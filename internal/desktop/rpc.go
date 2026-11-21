//go:build windows

package desktop

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"sync/atomic"
	"time"
)

// rpcError is the JSON-RPC error object returned by the Go backend.
type rpcError struct {
	Code    int    `json:"code"`
	Message string `json:"message"`
}

func (e *rpcError) Error() string {
	if e == nil {
		return ""
	}
	return fmt.Sprintf("%s (code %d)", e.Message, e.Code)
}

// rpcClient talks to the backend's POST /rpc endpoint. All requests are made by
// the host process; the page never opens a socket, it uses the WebView2 message
// bridge instead.
type rpcClient struct {
	endpoint string
	client   *http.Client
	nextID   atomic.Int64
}

func newRPCClient(addr string) *rpcClient {
	return &rpcClient{
		endpoint: "http://" + addr + "/rpc",
		client:   &http.Client{Timeout: 200 * time.Second},
	}
}

type rpcRequest struct {
	JSONRPC string          `json:"jsonrpc"`
	Method  string          `json:"method"`
	Params  json.RawMessage `json:"params,omitempty"`
	ID      int64           `json:"id"`
}

type rpcResponse struct {
	Result json.RawMessage `json:"result"`
	Error  *rpcError       `json:"error"`
	ID     int64           `json:"id"`
}

// Call invokes method with params (any JSON-marshalable value or raw bytes) and
// returns the raw result.
func (c *rpcClient) Call(ctx context.Context, method string, params any) (json.RawMessage, error) {
	var raw json.RawMessage
	switch v := params.(type) {
	case nil:
		raw = json.RawMessage(`{}`)
	case json.RawMessage:
		raw = v
	case []byte:
		raw = append(json.RawMessage(nil), v...)
	default:
		b, err := json.Marshal(v)
		if err != nil {
			return nil, fmt.Errorf("encode params: %w", err)
		}
		raw = b
	}
	if len(raw) == 0 || string(raw) == "null" {
		raw = json.RawMessage(`{}`)
	}

	body, err := json.Marshal(rpcRequest{
		JSONRPC: "2.0",
		Method:  method,
		Params:  raw,
		ID:      c.nextID.Add(1),
	})
	if err != nil {
		return nil, fmt.Errorf("encode request: %w", err)
	}

	req, err := http.NewRequestWithContext(ctx, http.MethodPost, c.endpoint, bytesReader(body))
	if err != nil {
		return nil, err
	}
	req.Header.Set("Content-Type", "application/json")

	resp, err := c.client.Do(req)
	if err != nil {
		return nil, fmt.Errorf("连接后端失败: %w", err)
	}
	defer resp.Body.Close()
	payload, err := io.ReadAll(io.LimitReader(resp.Body, 32<<20))
	if err != nil {
		return nil, fmt.Errorf("读取后端响应失败: %w", err)
	}
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("后端返回 HTTP %d", resp.StatusCode)
	}
	var rpcResp rpcResponse
	if err := json.Unmarshal(payload, &rpcResp); err != nil {
		return nil, fmt.Errorf("解析后端响应失败: %w", err)
	}
	if rpcResp.Error != nil {
		return nil, rpcResp.Error
	}
	return rpcResp.Result, nil
}

// bytesReader avoids importing bytes just for one call site.
func bytesReader(b []byte) io.Reader { return &sliceReader{b: b} }

type sliceReader struct {
	b []byte
	i int
}

func (r *sliceReader) Read(p []byte) (int, error) {
	if r.i >= len(r.b) {
		return 0, io.EOF
	}
	n := copy(p, r.b[r.i:])
	r.i += n
	return n, nil
}
