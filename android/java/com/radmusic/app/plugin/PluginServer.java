package com.radmusic.app.plugin;

import org.json.JSONObject;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.Charset;
import java.util.HashMap;
import java.util.Map;

/**
 * 插件 HTTP 服务（跑在 app 进程内的线程里，只监听 127.0.0.1）。
 *
 * 目的：让 C++ 服务端继续用「HTTP JSON-RPC 调插件」这套既有协议，
 * 而插件实现换成 Java —— 协议没变，服务端零改动。
 *
 * 端点：
 *   GET  /health          探活（宿主启动时 ping）
 *   POST /request         {"op":"search","params":{...}} → {"code":0,"data":{...}}
 *   GET  /media?u=...     媒体流代理（bilibili 音频校验 Referer，必须由这里代转）
 */
final class PluginServer {

    private static final Charset UTF8 = Charset.forName("UTF-8");
    private static final int MAX_BODY = 4 * 1024 * 1024;

    private final int port;
    private final Source source;
    private ServerSocket serverSocket;
    private Thread acceptThread;
    private volatile boolean running;

    PluginServer(int port, Source source) {
        this.port = port;
        this.source = source;
    }

    int port() {
        return port;
    }

    synchronized void start() throws IOException {
        if (running) return;
        serverSocket = new ServerSocket();
        serverSocket.setReuseAddress(true);
        serverSocket.bind(new InetSocketAddress(InetAddress.getByName("127.0.0.1"), port), 16);
        running = true;
        acceptThread = new Thread(new Runnable() {
            @Override
            public void run() {
                acceptLoop();
            }
        }, "plugin-" + source.platform());
        acceptThread.setDaemon(true);
        acceptThread.start();
        Http.log("插件已监听 127.0.0.1:" + port + " (" + source.platform() + ")");
    }

    synchronized void stop() {
        running = false;
        if (serverSocket != null) {
            try {
                serverSocket.close();
            } catch (IOException ignored) {
                // 关闭失败无妨
            }
            serverSocket = null;
        }
    }

    private void acceptLoop() {
        while (running) {
            try {
                final Socket socket = serverSocket.accept();
                Thread worker = new Thread(new Runnable() {
                    @Override
                    public void run() {
                        handle(socket);
                    }
                }, "plugin-req");
                worker.setDaemon(true);
                worker.start();
            } catch (IOException e) {
                if (running) Http.log("accept 失败：" + e.getMessage());
                else break;
            }
        }
    }

    private void handle(Socket socket) {
        try {
            socket.setSoTimeout(Http.TIMEOUT);
            BufferedInputStream in = new BufferedInputStream(socket.getInputStream());
            OutputStream rawOut = new BufferedOutputStream(socket.getOutputStream(), 64 * 1024);

            String head = readHead(in);
            if (head == null || head.isEmpty()) return;

            String[] lines = head.split("\r\n");
            String[] requestLine = lines[0].split(" ");
            if (requestLine.length < 3) return;
            String method = requestLine[0];
            String target = requestLine[1];

            Map<String, String> headers = new HashMap<>();
            for (int i = 1; i < lines.length; i++) {
                int colon = lines[i].indexOf(':');
                if (colon > 0) {
                    headers.put(lines[i].substring(0, colon).trim().toLowerCase(),
                            lines[i].substring(colon + 1).trim());
                }
            }

            String path = target;
            String query = "";
            int question = target.indexOf('?');
            if (question >= 0) {
                path = target.substring(0, question);
                query = target.substring(question + 1);
            }

            if ("/health".equals(path)) {
                JSONObject data = new JSONObject();
                data.put("ok", true);
                data.put("platform", source.platform());
                data.put("version", "java-1.0");
                writeJson(rawOut, 0, "ok", data);
                return;
            }

            if ("/media".equals(path)) {
                handleMedia(rawOut, query, headers);
                return;
            }

            if ("/request".equals(path) && "POST".equalsIgnoreCase(method)) {
                int length = parseInt(headers.get("content-length"), 0);
                byte[] body = readBody(in, length);
                String text = body.length == 0 ? "{}" : new String(body, UTF8);
                JSONObject payload;
                try {
                    payload = new JSONObject(text);
                } catch (Exception e) {
                    writeJson(rawOut, 1007, "请求体不是合法 JSON", null);
                    return;
                }
                String op = payload.optString("op", "");
                JSONObject params = payload.optJSONObject("params");
                if (params == null) params = new JSONObject();
                try {
                    Object result = source.request(op, params);
                    writeJsonRaw(rawOut, 0, "ok", result);
                } catch (Source.PluginError e) {
                    writeJson(rawOut, e.code, e.getMessage(), null);
                } catch (Exception e) {
                    Http.log("处理 " + op + " 失败：" + e);
                    writeJson(rawOut, 1011, "插件内部错误：" + e.getMessage(), null);
                }
                return;
            }

            writeJson(rawOut, 1011, "未知接口: " + path, null);
        } catch (Exception e) {
            Http.log("处理请求异常：" + e);
        } finally {
            try {
                socket.close();
            } catch (IOException ignored) {
                // 忽略
            }
        }
    }

    // ------------------------------------------------------------ 媒体代理

    private void handleMedia(OutputStream out, String query, Map<String, String> headers) throws IOException {
        Map<String, String> params = parseQuery(query);
        String encoded = params.get("u");
        if (encoded == null || encoded.isEmpty()) {
            writeText(out, 400, "missing url");
            return;
        }
        String upstream;
        try {
            upstream = new String(android.util.Base64.decode(encoded,
                    android.util.Base64.URL_SAFE | android.util.Base64.NO_PADDING | android.util.Base64.NO_WRAP), UTF8);
        } catch (Exception e) {
            writeText(out, 400, "bad url");
            return;
        }
        // 只允许白名单域名，避免被当成任意代理
        if (!allowedHost(upstream)) {
            writeText(out, 403, "host not allowed");
            return;
        }

        Map<String, String> reqHeaders = Http.headers(
                "User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36",
                "Referer", upstream.contains("bilibili") || upstream.contains("hdslb")
                        ? "https://www.bilibili.com/" : "https://music.163.com/");
        String range = headers.get("range");
        if (range != null) reqHeaders.put("Range", range);

        ByteArrayOutputStream buffer = new ByteArrayOutputStream();
        Http.RangeInfo info = new Http.RangeInfo();
        try {
            Http.pipe(upstream, reqHeaders, buffer, info);
        } catch (Exception e) {
            writeText(out, 502, "upstream failed: " + e.getMessage());
            return;
        }

        StringBuilder response = new StringBuilder();
        int status = info.status == 206 ? 206 : 200;
        response.append("HTTP/1.1 ").append(status).append(status == 206 ? " Partial Content\r\n" : " OK\r\n");
        response.append("Content-Type: ").append(info.contentType == null ? "application/octet-stream" : info.contentType)
                .append("\r\n");
        if (info.contentLength >= 0) response.append("Content-Length: ").append(info.contentLength).append("\r\n");
        if (info.contentRange != null) response.append("Content-Range: ").append(info.contentRange).append("\r\n");
        response.append("Accept-Ranges: bytes\r\n");
        response.append("Access-Control-Allow-Origin: *\r\n");
        response.append("Connection: close\r\n\r\n");
        out.write(response.toString().getBytes(UTF8));
        buffer.writeTo(out);
        out.flush();
    }

    private static boolean allowedHost(String url) {
        return url.contains("bilivideo.com") || url.contains("hdslb.com") || url.contains("bilibili.com")
                || url.contains("126.net") || url.contains("127.net") || url.contains("music.163.com")
                || url.contains("migu") || url.contains(".qq.com");
    }

    // ------------------------------------------------------------ HTTP 细节

    private static String readHead(InputStream in) throws IOException {
        ByteArrayOutputStream buffer = new ByteArrayOutputStream();
        int matched = 0;
        while (matched < 4) {
            int ch = in.read();
            if (ch < 0) return null;
            buffer.write(ch);
            if ((matched == 0 || matched == 2) && ch == '\r') matched++;
            else if ((matched == 1 || matched == 3) && ch == '\n') matched++;
            else matched = ch == '\r' ? 1 : 0;
            if (buffer.size() > 64 * 1024) return null;
        }
        String head = new String(buffer.toByteArray(), UTF8);
        return head.substring(0, head.length() - 4);
    }

    private static byte[] readBody(InputStream in, int length) throws IOException {
        if (length <= 0 || length > MAX_BODY) return new byte[0];
        byte[] data = new byte[length];
        int read = 0;
        while (read < length) {
            int got = in.read(data, read, length - read);
            if (got < 0) break;
            read += got;
        }
        return data;
    }

    private static Map<String, String> parseQuery(String query) {
        Map<String, String> map = new HashMap<>();
        if (query == null || query.isEmpty()) return map;
        for (String pair : query.split("&")) {
            int eq = pair.indexOf('=');
            if (eq <= 0) continue;
            try {
                map.put(java.net.URLDecoder.decode(pair.substring(0, eq), "UTF-8"),
                        java.net.URLDecoder.decode(pair.substring(eq + 1), "UTF-8"));
            } catch (Exception ignored) {
                // 跳过坏参数
            }
        }
        return map;
    }

    private static int parseInt(String text, int fallback) {
        if (text == null) return fallback;
        try {
            return Integer.parseInt(text.trim());
        } catch (NumberFormatException e) {
            return fallback;
        }
    }

    private static void writeJson(OutputStream out, int code, String message, JSONObject data) throws IOException {
        writeJsonRaw(out, code, message, data);
    }

    private static void writeJsonRaw(OutputStream out, int code, String message, Object data) throws IOException {
        JSONObject body = new JSONObject();
        try {
            body.put("code", code);
            body.put("message", message);
            if (data != null) body.put("data", data);
        } catch (Exception ignored) {
            // 忽略
        }
        byte[] bytes = body.toString().getBytes(UTF8);
        StringBuilder head = new StringBuilder();
        head.append("HTTP/1.1 200 OK\r\n");
        head.append("Content-Type: application/json; charset=utf-8\r\n");
        head.append("Content-Length: ").append(bytes.length).append("\r\n");
        head.append("Access-Control-Allow-Origin: *\r\n");
        head.append("Connection: close\r\n\r\n");
        out.write(head.toString().getBytes(UTF8));
        out.write(bytes);
        out.flush();
    }

    private static void writeText(OutputStream out, int status, String text) throws IOException {
        byte[] bytes = text.getBytes(UTF8);
        StringBuilder head = new StringBuilder();
        head.append("HTTP/1.1 ").append(status).append(" Error\r\n");
        head.append("Content-Type: text/plain; charset=utf-8\r\n");
        head.append("Content-Length: ").append(bytes.length).append("\r\n");
        head.append("Connection: close\r\n\r\n");
        out.write(head.toString().getBytes(UTF8));
        out.write(bytes);
        out.flush();
    }
}
