//go:build integration

// 端到端集成测试（子进程黑盒）。本文件带 build tag `integration`，默认不编译、不运行。
//
// 设计：编译 beatportdl.exe → 启动 `--serve --addr 127.0.0.1:<随机端口>` 子进程
// → 用 HTTP/JSON-RPC 驱动（getLatest/download/startScheduler）→ 轮询下载目录
// 断言文件出现 → 用 integrity.Checker 校验音频可解码 → 验证按曲风归档的子目录结构。
// 子进程自己做 OAuth 认证（用配置文件里的凭据或缓存），测试不直接构造 Beatport 客户端，
// 因此不 import package main（Go 不允许），保持 test/ 与源码隔离。
//
// 启用条件：CGO 工具链 + TagLib C 库 + Beatport 凭据 + 可访问的网络。
//
// 启用方式：
//
//	1. 设置环境变量提供 Beatport 凭据（仅作门控，不直接传给 exe）：
//	     BPDL_E2E_USERNAME=你的Beatport用户名
//	     BPDL_E2E_PASSWORD=你的Beatport密码
//	2. 运行（需先 . .\setenv.ps1 配好 CGO 环境）：
//	     CGO_ENABLED=1 go test -tags integration -v ./test/e2e/
//
// 隔离机制：startServer 用 t.TempDir() 生成临时目录，写入含凭据+下载目录
// 的 config.yml，用 --config 指定给 exe。FindCacheFile 让凭据缓存与配置
// 同目录，因此配置/缓存/下载产物全部隔离于本机。测试结束 t.TempDir 自动清理。
// OAuth 首次登录会在临时目录产生缓存；若凭据有效，同目录的缓存可复用。
//
// 注意：真实下载会消耗 Beatport 流量配额，仅在必要时运行。
package e2e_test

import (
	"bytes"
	"encoding/json"
	"io"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"

	"unspok3n/beatportdl/internal/integrity"
)

// envOrSkip 跳过测试当所需环境变量未设置时，避免 CI 误跑真实 API。
func envOrSkip(t *testing.T, key string) string {
	t.Helper()
	v := os.Getenv(key)
	if v == "" {
		t.Skipf("set %s to run this end-to-end test", key)
	}
	return v
}

// freePort 让操作系统分配一个空闲端口，避免与已运行的服务冲突。
func freePort(t *testing.T) int {
	t.Helper()
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen for free port: %v", err)
	}
	defer l.Close()
	return l.Addr().(*net.TCPAddr).Port
}

// serverFixture 启动一个 --serve 子进程并暴露其 JSON-RPC 端点。
// 子进程用 --config 指向一个临时配置文件，凭据缓存也写到该配置同目录，
// 完全隔离本机的配置与缓存。下载目录也是临时的。
type serverFixture struct {
	cmd      *exec.Cmd
	baseURL  string
	dlDir    string
	cfgDir   string // 临时配置+缓存所在目录，cleanup 时整目录删除
	cfgPath  string
	exePath  string
	stdout   *bytes.Buffer
	stderr   *bytes.Buffer
}

// startServer 编译（若需要）并启动 beatportdl.exe --serve --config <tmp> 子进程。
// 等待 /health 就绪或超时。返回 fixture 与 cleanup。
//
// 隔离机制：生成临时配置目录（t.TempDir），写入含凭据+下载目录的 config.yml，
// 用 --config 指定。FindCacheFile 会让凭据缓存与配置同目录，因此缓存也隔离。
// 测试结束 cleanup 删除整个临时目录（配置+缓存+下载产物若在配置同目录）。
func startServer(t *testing.T) (*serverFixture, func()) {
	t.Helper()
	user := envOrSkip(t, "BPDL_E2E_USERNAME")
	pass := envOrSkip(t, "BPDL_E2E_PASSWORD")
	return startServerWithCreds(t, user, pass)
}

// startServerNoCreds 用无凭据配置启动 --serve 子进程，用于测试 UI 登录流程
// （authStatus 应返回 ready=false，login 用假凭据应失败）。不门控环境变量，
// 无需 Beatport 凭据即可运行，验证的是 RPC 链路而非真实认证。
func startServerNoCreds(t *testing.T) (*serverFixture, func()) {
	t.Helper()
	return startServerWithCreds(t, "", "")
}

// startServerWithCreds 是 startServer/startServerNoCreds 的共用实现。
// user/pass 为空时配置不含凭据（--serve 的 ParseLenient 允许）。
func startServerWithCreds(t *testing.T, user, pass string) (*serverFixture, func()) {
	t.Helper()

	root, _ := os.Getwd()
	moduleRoot := filepath.Join(root, "..", "..")
	exePath := filepath.Join(moduleRoot, "bin", "beatportdl.exe")

	// 若二进制不存在，尝试就地编译（要求 CGO 环境已配好）。
	if _, err := os.Stat(exePath); os.IsNotExist(err) {
		t.Logf("beatportdl.exe not found, building...")
		build := exec.Command("go", "build", "-o", exePath, "./cmd/beatportdl")
		build.Dir = moduleRoot
		if out, err := build.CombinedOutput(); err != nil {
			t.Fatalf("build beatportdl.exe: %v\n%s", err, out)
		}
	}

	// 临时配置目录：配置文件 + 凭据缓存 + 下载目录都放这里，一次清理。
	cfgDir := t.TempDir()
	cfgPath := filepath.Join(cfgDir, "beatportdl-config.yml")
	dlDir := filepath.Join(cfgDir, "downloads")
	if err := os.MkdirAll(dlDir, 0o755); err != nil {
		t.Fatalf("mkdir dlDir: %v", err)
	}
	// 生成临时配置：凭据可空（--serve 的 ParseLenient 允许），下载目录
	// 指向临时目录，开启端到端骨架要验证的新功能。
	var cfgYAML string
	if user != "" && pass != "" {
		cfgYAML = "username: " + user + "\n" +
			"password: " + pass + "\n"
	}
	cfgYAML += "downloads_directory: " + filepath.ToSlash(dlDir) + "\n" +
		"quality: lossless\n" +
		"sort_by_genre: true\n" +
		"genre_sort_mode: genre\n" +
		"verify_integrity: true\n"
	if err := os.WriteFile(cfgPath, []byte(cfgYAML), 0o600); err != nil {
		t.Fatalf("write temp config: %v", err)
	}

	port := freePort(t)
	addr := "127.0.0.1:" + strconv.Itoa(port)

	// 子进程用 --config 指向临时配置，--serve + --addr 指定端口。
	cmd := exec.Command(exePath, "--config="+cfgPath, "--serve", "--addr", addr)
	cmd.Dir = moduleRoot
	// 继承当前环境（CGO 运行需要 MSYS2 MinGW DLL 在 PATH 里）。
	cmd.Env = os.Environ()
	stdoutBuf := &bytes.Buffer{}
	stderrBuf := &bytes.Buffer{}
	cmd.Stdout = stdoutBuf
	cmd.Stderr = stderrBuf
	if err := cmd.Start(); err != nil {
		t.Fatalf("start server: %v", err)
	}

	baseURL := "http://" + addr
	// 等 /health 就绪。
	deadline := time.Now().Add(30 * time.Second)
	ready := false
	for time.Now().Before(deadline) {
		resp, err := http.Get(baseURL + "/health")
		if err == nil && resp.StatusCode == 200 {
			resp.Body.Close()
			ready = true
			break
		}
		if resp != nil {
			resp.Body.Close()
		}
		time.Sleep(200 * time.Millisecond)
	}
	if !ready {
		cmd.Process.Kill()
		t.Fatalf("server did not become ready within 30s\nstdout:\n%s\nstderr:\n%s", stdoutBuf.String(), stderrBuf.String())
	}

	f := &serverFixture{
		cmd:     cmd,
		baseURL: baseURL,
		dlDir:   dlDir,
		cfgDir:  cfgDir,
		cfgPath: cfgPath,
		exePath: exePath,
		stdout:  stdoutBuf,
		stderr:  stderrBuf,
	}
	cleanup := func() {
		if cmd.Process != nil {
			_ = cmd.Process.Kill()
			_, _ = cmd.Process.Wait()
		}
	}
	return f, cleanup
}

// rpc 调用 JSON-RPC 方法。返回 result 部分。
func (f *serverFixture) rpc(t *testing.T, method string, params map[string]interface{}) json.RawMessage {
	t.Helper()
	body, _ := json.Marshal(map[string]interface{}{
		"method": method,
		"params": params,
		"id":     1,
	})
	resp, err := http.Post(f.baseURL+"/rpc", "application/json", bytes.NewReader(body))
	if err != nil {
		t.Fatalf("rpc %s: %v", method, err)
	}
	defer resp.Body.Close()
	raw, _ := io.ReadAll(resp.Body)
	var env struct {
		Result json.RawMessage `json:"result"`
		Error  *struct {
			Code    int    `json:"code"`
			Message string `json:"message"`
		} `json:"error"`
	}
	if err := json.Unmarshal(raw, &env); err != nil {
		t.Fatalf("rpc %s: decode %v (raw=%s)", method, err, raw)
	}
	if env.Error != nil {
		t.Fatalf("rpc %s error [%d]: %s", method, env.Error.Code, env.Error.Message)
	}
	return env.Result
}

// rpcExpectError 调用 JSON-RPC 方法，期望返回 error。返回错误消息，
// 供断言。若调用成功（无 error）则 t.Fatal。
func (f *serverFixture) rpcExpectError(t *testing.T, method string, params map[string]interface{}) string {
	t.Helper()
	body, _ := json.Marshal(map[string]interface{}{
		"method": method,
		"params": params,
		"id":     1,
	})
	resp, err := http.Post(f.baseURL+"/rpc", "application/json", bytes.NewReader(body))
	if err != nil {
		t.Fatalf("rpc %s: %v", method, err)
	}
	defer resp.Body.Close()
	raw, _ := io.ReadAll(resp.Body)
	var env struct {
		Result json.RawMessage `json:"result"`
		Error  *struct {
			Code    int    `json:"code"`
			Message string `json:"message"`
		} `json:"error"`
	}
	if err := json.Unmarshal(raw, &env); err != nil {
		t.Fatalf("rpc %s: decode %v (raw=%s)", method, err, raw)
	}
	if env.Error == nil {
		t.Fatalf("rpc %s: expected error but got result=%s", method, string(env.Result))
	}
	return env.Error.Message
}

// waitForFile 轮询下载目录，等待至少一个音频文件出现或超时。
func waitForFile(t *testing.T, dir string, timeout time.Duration) string {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		var found string
		filepath.Walk(dir, func(p string, info os.FileInfo, err error) error {
			if err != nil || info.IsDir() {
				return nil
			}
			ext := strings.ToLower(filepath.Ext(p))
			if ext == ".flac" || ext == ".m4a" {
				found = p
				return filepath.SkipDir
			}
			return nil
		})
		if found != "" {
			return found
		}
		time.Sleep(500 * time.Millisecond)
	}
	return ""
}

// TestE2E_ServerMode_DownloadLatestViaRPC 是最完整的端到端用例：
// 启动 --serve 子进程 → getLatest 拉最新曲目 → download 入队 → 轮询下载目录
// → 用 integrity.Checker 校验音频可解码 → 验证按曲风归档的子目录结构。
//
// 本骨架当前依赖真实 Beatport 凭据与网络，且下载会消耗配额。未设凭据时
// 通过 envOrSkip 跳过；设了凭据但 API/下载失败的诊断信息会打印到 t.Log。
func TestE2E_ServerMode_DownloadLatestViaRPC(t *testing.T) {
	f, cleanup := startServer(t)
	defer cleanup()

	// 1. 拉取最新曲目（取少量，避免配额）。
	res := f.rpc(t, "getLatest", map[string]interface{}{
		"per_page": 3,
		"kind":     "tracks",
	})
	var latest struct {
		Results []struct {
			ID    int64  `json:"id"`
			Name  string `json:"name"`
			URL   string `json:"url"`
			Genre struct{ Name string } `json:"genre"`
		} `json:"results"`
		Page    int  `json:"page"`
		HasMore bool `json:"has_more"`
	}
	if err := json.Unmarshal(res, &latest); err != nil {
		t.Fatalf("decode latest tracks: %v (raw=%s)", err, res)
	}
	tracks := latest.Results
	if len(tracks) == 0 {
		t.Skip("no latest tracks returned; nothing to test")
	}
	t.Logf("latest track: %s [%s] url=%s", tracks[0].Name, tracks[0].Genre.Name, tracks[0].URL)

	// 2. 入队下载第一首。
	_ = f.rpc(t, "download", map[string]interface{}{
		"urls":     []string{tracks[0].URL},
		"genre_dir": true,
	})

	// 3. 轮询下载目录等待文件出现（给真实下载留足时间）。
	file := waitForFile(t, f.dlDir, 5*time.Minute)
	if file == "" {
		t.Fatalf("no audio file appeared in %s within 5m\nstdout:\n%s", f.dlDir, f.stdout.String())
	}
	t.Logf("downloaded: %s", file)

	// 4. 完整性校验：文件必须可被 TagLib 解码且采样率 >0。
	checker := integrity.NewChecker()
	verdict := checker.VerifyAudio(file)
	if !verdict.Valid {
		t.Fatalf("integrity check failed: %s (sample=%d, bytes=%d)", verdict.Error, verdict.SampleRate, verdict.ActualBytes)
	}
	t.Logf("verified: %d bytes, %d Hz", verdict.ActualBytes, verdict.SampleRate)

	// 5. 按曲风归档验证：文件应位于 <dlDir>/<genre>/ 下。
	rel, err := filepath.Rel(f.dlDir, file)
	if err != nil {
		t.Fatalf("Rel: %v", err)
	}
	parts := strings.Split(filepath.ToSlash(rel), "/")
	if len(parts) < 2 {
		t.Fatalf("file not under genre subdirectory: %s", rel)
	}
	genreDir := parts[0]
	if genreDir == "" || genreDir == "." {
		t.Fatalf("file at downloads root, not filed under genre: %s", rel)
	}
	t.Logf("filed under genre dir: %s", genreDir)
}

// TestE2E_Scheduler_SingleScan_ViaRPC 验证定时扫描骨架：
// 启动 --serve → startScheduler → 等 scheduler.found SSE 事件 → 验证有候选 URL。
//
// 本骨架只验证扫描能拿到曲目并广播事件；真正下载由上面的 Download 用例覆盖。
func TestE2E_Scheduler_SingleScan_ViaRPC(t *testing.T) {
	f, cleanup := startServer(t)
	defer cleanup()

	// 订阅 SSE 事件流。
	resp, err := http.Get(f.baseURL + "/events")
	if err != nil {
		t.Fatalf("subscribe events: %v", err)
	}
	defer resp.Body.Close()

	// 启动 scheduler（30s 间隔，单测不会触发第二轮）。
	_ = f.rpc(t, "startScheduler", map[string]interface{}{
		"interval_seconds": 30,
		"per_page":         3,
	})

	// 读 SSE 流，等待 scheduler.found 事件。
	buf := make([]byte, 4096)
	deadline := time.Now().Add(60 * time.Second)
	for time.Now().Before(deadline) {
		n, err := resp.Body.Read(buf)
		if err != nil {
			t.Fatalf("read SSE: %v", err)
		}
		chunk := string(buf[:n])
		if strings.Contains(chunk, "scheduler.found") {
			t.Logf("scheduler fired: %s", strings.TrimSpace(chunk))
			return
		}
	}
	t.Fatal("scheduler did not fire within 60s")
}

// TestE2E_LoginFlow_RPC 验证 UI 登录流程的 RPC 链路：用无凭据配置启动
// --serve → authStatus 应返回 ready=false → 用假凭据调 login 应失败
// → authStatus 仍 ready=false。
//
// 本测试不依赖真实 Beatport 凭据（用假凭据，期望失败），验证的是：
//   - --serve 在无凭据配置下能启动（ParseLenient 生效，不 Fatal）
//   - authStatus RPC 端到端可达，正确报告未认证状态
//   - login RPC 端到端可达，能区分成功/失败并把错误回传给前端
//   - 登录失败不会意外把 authReady 置 true
//
// 真实凭据登录成功路径由 TestE2E_ServerMode_DownloadLatestViaRPC 覆盖
// （需 BPDL_E2E_USERNAME/PASSWORD）。
func TestE2E_LoginFlow_RPC(t *testing.T) {
	f, cleanup := startServerNoCreds(t)
	defer cleanup()

	// 1. 初始状态：无凭据配置启动，authStatus 应 ready=false。
	authRaw := f.rpc(t, "authStatus", map[string]interface{}{})
	var st struct {
		Ready bool   `json:"ready"`
		Err   string `json:"error,omitempty"`
	}
	if err := json.Unmarshal(authRaw, &st); err != nil {
		t.Fatalf("decode authStatus: %v (raw=%s)", err, authRaw)
	}
	if st.Ready {
		t.Fatalf("authStatus: expected ready=false on no-credential startup, got ready=true")
	}
	t.Logf("authStatus (initial): ready=%v, err=%q", st.Ready, st.Err)

	// 2. 用假凭据调 login，应失败（凭据无效/网络错误，但 RPC 链路通）。
	errMsg := f.rpcExpectError(t, "login", map[string]interface{}{
		"username": "fake_e2e_user",
		"password": "fake_e2e_pass_12345",
	})
	if errMsg == "" {
		t.Fatal("login with fake credentials: expected non-empty error message")
	}
	t.Logf("login (fake creds): error=%q", errMsg)

	// 3. 登录失败后 authStatus 仍 ready=false（不能误置 true）。
	authRaw2 := f.rpc(t, "authStatus", map[string]interface{}{})
	var st2 struct {
		Ready bool   `json:"ready"`
		Err   string `json:"error,omitempty"`
	}
	if err := json.Unmarshal(authRaw2, &st2); err != nil {
		t.Fatalf("decode authStatus after failed login: %v (raw=%s)", err, authRaw2)
	}
	if st2.Ready {
		t.Fatal("authStatus after failed login: expected ready=false, got ready=true (failed login must not flip authReady)")
	}
	t.Logf("authStatus (after failed login): ready=%v", st2.Ready)
}
