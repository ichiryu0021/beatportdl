//go:build windows

package desktop

import (
	"fmt"
	"net/url"
	"os"
	"strings"

	"gopkg.in/yaml.v2"
)

// readConfigString reads a top-level string value from the YAML config without
// disturbing the rest of the file (the backend owns the schema; the shell only
// edits the `proxy` key).
func readConfigString(path, key string) string {
	data, err := os.ReadFile(path)
	if err != nil {
		return ""
	}
	var values map[string]interface{}
	if err := yaml.Unmarshal(data, &values); err != nil {
		return ""
	}
	raw, ok := values[key]
	if !ok || raw == nil {
		return ""
	}
	text, _ := raw.(string)
	return strings.TrimSpace(text)
}

// writeConfigString replaces (or appends) a top-level `key: value` line while
// preserving every other line, including comments and nested mappings.
func writeConfigString(path, key, value string) error {
	escaped := strings.ReplaceAll(value, "'", "''")
	return writeConfigLine(path, key, fmt.Sprintf("%s: '%s'", key, escaped))
}

// writeConfigBool writes a top-level YAML boolean.
func writeConfigBool(path, key string, value bool) error {
	return writeConfigLine(path, key, fmt.Sprintf("%s: %t", key, value))
}

// readConfigBool reads a top-level boolean from the YAML config.
func readConfigBool(path, key string) bool {
	data, err := os.ReadFile(path)
	if err != nil {
		return false
	}
	var values map[string]interface{}
	if err := yaml.Unmarshal(data, &values); err != nil {
		return false
	}
	raw, ok := values[key]
	if !ok || raw == nil {
		return false
	}
	switch v := raw.(type) {
	case bool:
		return v
	case string:
		return strings.EqualFold(strings.TrimSpace(v), "true")
	default:
		return false
	}
}

// writeConfigLine rewrites the line for key with the already-rendered YAML
// value, or appends it when absent.
func writeConfigLine(path, key, replacement string) error {
	data, err := os.ReadFile(path)
	if err != nil {
		return fmt.Errorf("读取配置文件失败: %w", err)
	}
	text := string(data)
	lines := strings.Split(text, "\n")
	found := false
	for i, line := range lines {
		body := strings.TrimRight(line, "\r")
		if len(body) < len(key)+1 || !strings.HasPrefix(body, key+":") {
			continue
		}
		// Only top-level keys: no leading space/tab.
		if body[0] == ' ' || body[0] == '\t' {
			continue
		}
		rest := body[len(key)+1:]
		if rest != "" && rest[0] != ' ' {
			continue
		}
		cr := ""
		if strings.HasSuffix(line, "\r") {
			cr = "\r"
		}
		lines[i] = replacement + cr
		found = true
		break
	}
	if !found {
		if len(text) > 0 && !strings.HasSuffix(text, "\n") {
			lines = append(lines, "")
		}
		lines = append(lines, replacement)
	}
	out := strings.Join(lines, "\n")
	tmp := path + ".bpdl-tmp"
	if err := os.WriteFile(tmp, []byte(out), 0o600); err != nil {
		return fmt.Errorf("写入配置文件失败: %w", err)
	}
	if err := os.Rename(tmp, path); err != nil {
		_ = os.Remove(tmp)
		return fmt.Errorf("保存配置文件失败: %w", err)
	}
	return nil
}

// normalizeProxy validates a proxy URL and returns a canonical form. Empty
// input means "direct connection".
func normalizeProxy(raw string) (string, error) {
	raw = strings.TrimSpace(raw)
	if raw == "" {
		return "", nil
	}
	if !strings.Contains(raw, "://") {
		raw = "http://" + raw
	}
	parsed, err := url.Parse(raw)
	if err != nil {
		return "", fmt.Errorf("代理地址无效：%v", err)
	}
	if parsed.Host == "" {
		return "", fmt.Errorf("代理地址缺少主机和端口，示例：http://127.0.0.1:7890")
	}
	switch strings.ToLower(parsed.Scheme) {
	case "http", "https":
	case "socks5", "socks5h":
		parsed.Scheme = "socks5"
	default:
		return "", fmt.Errorf("仅支持 http / https / socks5 代理")
	}
	if parsed.Path != "" && parsed.Path != "/" {
		return "", fmt.Errorf("代理地址不能包含路径")
	}
	parsed.Path = ""
	parsed.RawPath = ""
	parsed.Fragment = ""
	return parsed.String(), nil
}

// maskProxy hides credentials before the URL is written to logs.
func maskProxy(raw string) string {
	if raw == "" {
		return "直连"
	}
	parsed, err := url.Parse(raw)
	if err != nil {
		return "(无效代理)"
	}
	if parsed.User != nil {
		parsed.User = url.User("***")
	}
	return parsed.String()
}
