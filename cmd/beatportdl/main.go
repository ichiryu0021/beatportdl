package main

import (
	"context"
	"flag"
	"fmt"
	"github.com/fatih/color"
	"github.com/vbauerster/mpb/v8"
	"io"
	"os"
	"os/signal"
	"strings"
	"sync"
	"syscall"
	"unspok3n/beatportdl/config"
	"unspok3n/beatportdl/internal/beatport"
	"unspok3n/beatportdl/internal/integrity"
	"unspok3n/beatportdl/internal/server"
	"unspok3n/beatportdl/internal/sorting"
)

const (
	configFilename = "beatportdl-config.yml"
	cacheFilename  = "beatportdl-credentials.json"
	errorFilename  = "beatportdl-err.log"
)

// configPathOverride 由 --config flag 设置（见 parseEarlyFlags），非空时
// FindConfigFile 直接返回该路径，跳过默认搜索顺序。用于端到端测试隔离
// 与多配置切换。
var configPathOverride string

// earlyServeFlag / earlyAddrFlag 与 --config 一样在 Setup() 之前解析，
// 因为认证段需要知道是否处于 --serve 模式（决定是否延迟认证）。
var (
	earlyServeFlag bool
	earlyAddrFlag  string
)

type application struct {
	config      *config.AppConfig
	configPath  string // 配置文件路径，login 成功后写回凭据用
	logFile     *os.File
	logWriter   io.Writer
	ctx         context.Context
	wg          sync.WaitGroup
	downloadSem chan struct{}
	globalSem   chan struct{}
	pbp         *mpb.Progress

	urls             []string
	activeFiles      map[string]struct{}
	activeFilesMutex sync.RWMutex

	bp  *beatport.Beatport
	srv *server.Server

	checker  *integrity.Checker
	archiver *sorting.Archiver

	authMu    sync.RWMutex
	authReady bool // --serve 模式下，缓存失效时不 Fatal 而是标记 false，等前端 login RPC
	auth      *beatport.Auth
}

func main() {
	// 提前解析 --config：必须在 Setup() 之前生效，因为 Setup 会读配置文件
	// （不存在时还会交互式创建）。识别后从 os.Args 移除该 flag 及其值，避免
	// 与下方全局 flag.Parse 冲突（flag.Parse 遇到未知 flag 会报错）。
	earlyArgs, restArgs := splitEarlyArgs(os.Args[1:], "config", "serve", "addr")
	earlyFlags := flag.NewFlagSet("early", flag.ContinueOnError)
	earlyFlags.SetOutput(io.Discard)
	earlyFlags.StringVar(&configPathOverride, "config", "", "Path to beatportdl-config.yml (overrides default search)")
	earlyFlags.BoolVar(&earlyServeFlag, "serve", false, "Run the local HTTP/JSON-RPC server for the GUI instead of the CLI prompt")
	earlyFlags.StringVar(&earlyAddrFlag, "addr", "127.0.0.1:8765", "Listen address for the --serve HTTP/JSON-RPC server")
	_ = earlyFlags.Parse(earlyArgs)
	os.Args = append([]string{os.Args[0]}, restArgs...)

	cfg, cachePath, err := Setup()
	if err != nil {
		fmt.Println(err.Error())
		Pause()
		return
	}

	ctx, cancel := context.WithCancel(context.Background())

	app := &application{
		config:      cfg,
		downloadSem: make(chan struct{}, cfg.MaxDownloadWorkers),
		globalSem:   make(chan struct{}, cfg.MaxGlobalWorkers),
		ctx:         ctx,
		logWriter:   os.Stdout,
	}
	// 配置文件路径：login 成功后要写回凭据，Setup 内部已用 FindConfigFile
	// 定位，这里再取一次复用同一路径。
	if cfgPath, _, cfgErr := FindConfigFile(); cfgErr == nil {
		app.configPath = cfgPath
	}

	go func() {
		sigCh := make(chan os.Signal, 1)
		signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)

		<-sigCh

		if len(app.urls) > 0 {
			app.LogInfo("Shutdown signal received. Waiting for download workers to finish")
			cancel()

			<-sigCh
		}

		os.Exit(0)
	}()

	if cfg.WriteErrorLog {
		logFilePath, _, err := FindErrorLogFile()
		if err != nil {
			fmt.Println(err.Error())
			Pause()
		}
		f, err := os.OpenFile(logFilePath, os.O_APPEND|os.O_WRONLY|os.O_CREATE, 0600)
		if err != nil {
			panic(err)
		}
		app.logFile = f
		defer f.Close()
	}

	auth := beatport.NewAuth(cfg.Username, cfg.Password, cachePath)
	bp := beatport.New(cfg.Proxy, auth)
	app.auth = auth

	if err := auth.LoadCache(); err != nil {
		// CLI 模式：缓存失效必须立刻登录，否则后续下载全失败。
		// --serve 模式：延迟认证——标记未认证，等前端 login RPC 提交凭据，
		// 避免 server 启动时因无凭据/网络问题直接 Fatal 退出。
		if earlyServeFlag {
			if cfg.Username != "" && cfg.Password != "" {
				if err := auth.Init(bp); err != nil {
					app.LogInfo("server: auth not ready: " + err.Error())
				} else {
					app.setAuthReady(true)
				}
			} else {
				app.LogInfo("server: no credentials in config; waiting for GUI login")
			}
		} else {
			if err := auth.Init(bp); err != nil {
				app.FatalError("beatport", err)
			}
		}
	} else {
		app.setAuthReady(true)
	}

	app.bp = bp
	// Integrity checker & genre archiver are wired up from config so both the
	// CLI and the server (GUI) modes benefit. A nil archiver keeps the legacy
	// (non-genre) directory layout untouched.
	app.checker = integrity.NewChecker()
	if cfg.SortByGenre {
		mode := sorting.ModeGenre
		if cfg.GenreSortMode == "subgenre_or_genre" {
			mode = sorting.ModeSubgenreOrGenre
		}
		app.archiver = sorting.NewArchiver(cfg.DownloadsDirectory, mode, cfg.WhitespaceCharacter)
	}
	quitFlag := flag.Bool("q", false, "Quit the main loop after finishing")

	flag.Parse()
	inputArgs := flag.Args()

	for _, arg := range inputArgs {
		if strings.HasSuffix(arg, ".txt") {
			app.parseTextFile(arg)
		} else {
			app.urls = append(app.urls, arg)
		}
	}

	// Server mode: expose the download pipeline over local JSON-RPC/SSE for a
	// GUI front-end and run a headless loop (no stdin prompt). serve/addr 已在
	// Setup() 之前提前解析（见上方 early flags）。
	if earlyServeFlag {
		if err := runServer(app, earlyAddrFlag); err != nil && ctx.Err() == nil {
			fmt.Println(err.Error())
		}
		app.serverShutdown(context.Background())
		return
	}

	for {
		if len(app.urls) == 0 {
			app.mainPrompt()
		}

		app.pbp = mpb.New(mpb.WithAutoRefresh(), mpb.WithOutput(color.Output))
		app.logWriter = app.pbp
		app.activeFiles = make(map[string]struct{}, len(app.urls))

		for _, url := range app.urls {
			app.globalWorker(func() {
				app.handleUrl(url)
			})
		}

		app.wg.Wait()
		app.pbp.Shutdown()

		if *quitFlag || ctx.Err() != nil {
			break
		}

		app.urls = []string{}
	}
}

// splitEarlyArgs 把 args 拆成两部分：
//   - early: 仅含名为 names 之一的 flag（支持 --name=X、-name=X、--name X、-name X）；
//   - rest:  去掉 early 部分后的剩余参数，供后续全局 flag.Parse 使用。
//
// 这样 --config/--serve/--addr 能在 Setup() 之前生效，又不会让后续 flag.Parse
// 因遇到未注册的 flag 而报错。
func splitEarlyArgs(args []string, names ...string) (early, rest []string) {
	// name -> 是否可能带值（bool flag 如 --serve 不带值，但本实现统一按
	// "可能带值"处理：若 --name 后跟一个非 flag 参数，就当它的值）。
	nameSet := make(map[string]bool, len(names))
	for _, n := range names {
		nameSet["--"+n] = true
		nameSet["-"+n] = true
	}
	isEarlyName := func(a string) (string, bool) {
		// 返回 (key, isLongShortForm)。先匹配 --name=X / -name=X。
		for _, n := range names {
			if strings.HasPrefix(a, "--"+n+"=") || strings.HasPrefix(a, "-"+n+"=") {
				return a, true
			}
		}
		if nameSet[a] {
			return a, false
		}
		return "", false
	}
	for i := 0; i < len(args); i++ {
		a := args[i]
		key, hasInlineVal := isEarlyName(a)
		if key == "" {
			rest = append(rest, a)
			continue
		}
		early = append(early, a)
		if hasInlineVal {
			// --name=X 形式，值已内联，无需再吞下一个 arg。
			continue
		}
		// --name 形式：若下一个 arg 不是另一个 flag，就当它的值吞掉。
		if i+1 < len(args) && !strings.HasPrefix(args[i+1], "-") {
			early = append(early, args[i+1])
			i++
		}
	}
	return early, rest
}
