package com.radmusic.app.plugin;

import android.util.Log;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.LinkedHashMap;
import java.util.Map;

/**
 * 极简 HTTP 工具：只做插件需要的事（GET 文本 / GET JSON / 流式转发）。
 *
 * 用系统的 HttpURLConnection 而不是自己写 socket —— 手机上的 HTTPS 交给系统处理，
 * 这样就不用交叉编译 TLS 库，也不用在 C++ 里带一份。
 */
final class Http {

    private static final String TAG = "RadMusicPlugin";
    static final int TIMEOUT = 20000;
    private static final int RETRIES = 2;

    private Http() {}

    static String get(String url, Map<String, String> headers) throws IOException {
        IOException last = null;
        for (int attempt = 0; attempt <= RETRIES; attempt++) {
            HttpURLConnection conn = null;
            try {
                conn = (HttpURLConnection) new URL(url).openConnection();
                conn.setConnectTimeout(TIMEOUT);
                conn.setReadTimeout(TIMEOUT);
                conn.setRequestMethod("GET");
                conn.setInstanceFollowRedirects(true);
                if (headers != null) {
                    for (Map.Entry<String, String> entry : headers.entrySet()) {
                        conn.setRequestProperty(entry.getKey(), entry.getValue());
                    }
                }
                final int code = conn.getResponseCode();
                final InputStream stream = code >= 400 ? conn.getErrorStream() : conn.getInputStream();
                final String body = stream == null ? "" : readAll(stream);
                if (code >= 400) {
                    // 412 是 bilibili 风控，-352 之类会出现在 body 里，交给上层判断
                    throw new HttpError(code, body);
                }
                return body;
            } catch (HttpError e) {
                throw e;
            } catch (IOException e) {
                last = e;
                if (attempt < RETRIES) sleep(400L * (attempt + 1));
            } finally {
                if (conn != null) conn.disconnect();
            }
        }
        throw last != null ? last : new IOException("请求失败");
    }

    /** POST 表单（网易云的 cloudsearch 等接口要求 form-urlencoded） */
    static String postForm(String url, java.util.Map<String, String> form, java.util.Map<String, String> headers)
            throws IOException {
        StringBuilder body = new StringBuilder();
        if (form != null) {
            for (java.util.Map.Entry<String, String> entry : form.entrySet()) {
                if (body.length() > 0) body.append('&');
                body.append(encode(entry.getKey())).append('=').append(encode(entry.getValue()));
            }
        }
        final byte[] payload = body.toString().getBytes("UTF-8");
        HttpURLConnection conn = null;
        try {
            conn = (HttpURLConnection) new URL(url).openConnection();
            conn.setConnectTimeout(TIMEOUT);
            conn.setReadTimeout(TIMEOUT);
            conn.setRequestMethod("POST");
            conn.setDoOutput(true);
            conn.setInstanceFollowRedirects(true);
            conn.setRequestProperty("Content-Type", "application/x-www-form-urlencoded; charset=utf-8");
            if (headers != null) {
                for (java.util.Map.Entry<String, String> entry : headers.entrySet()) {
                    conn.setRequestProperty(entry.getKey(), entry.getValue());
                }
            }
            OutputStream out = conn.getOutputStream();
            out.write(payload);
            out.close();
            final int code = conn.getResponseCode();
            final InputStream stream = code >= 400 ? conn.getErrorStream() : conn.getInputStream();
            final String text = stream == null ? "" : readAll(stream);
            if (code >= 400) throw new HttpError(code, text);
            return text;
        } finally {
            if (conn != null) conn.disconnect();
        }
    }

    static JSONObject postJson(String url, java.util.Map<String, String> form,
                               java.util.Map<String, String> headers) throws IOException {
        final String body = postForm(url, form, headers);
        try {
            return new JSONObject(body);
        } catch (Exception e) {
            throw new IOException("返回的不是合法 JSON（长度 " + body.length() + "）");
        }
    }

    private static String encode(String text) {
        try {
            return java.net.URLEncoder.encode(text == null ? "" : text, "UTF-8");
        } catch (Exception e) {
            return text;
        }
    }

    static JSONObject getJson(String url, Map<String, String> headers) throws IOException {
        final String body = get(url, headers);
        try {
            return new JSONObject(body);
        } catch (Exception e) {
            throw new IOException("返回的不是合法 JSON（长度 " + body.length() + "）");
        }
    }

    /**
     * 把上游媒体流原样转发到客户端（音频播放用）。
     * bilibili 的音频 CDN 校验 Referer，必须由插件代转，不能让 WebView 直连。
     * 支持 Range：客户端拖动进度条时要把 Range 透传给上游，并把 206 与 Content-Range 原样回传。
     */
    static void pipe(String url, Map<String, String> headers, OutputStream out, RangeInfo range)
            throws IOException {
        HttpURLConnection conn = (HttpURLConnection) new URL(url).openConnection();
        conn.setConnectTimeout(TIMEOUT);
        conn.setReadTimeout(TIMEOUT);
        conn.setRequestMethod("GET");
        conn.setInstanceFollowRedirects(true);
        if (headers != null) {
            for (Map.Entry<String, String> entry : headers.entrySet()) {
                conn.setRequestProperty(entry.getKey(), entry.getValue());
            }
        }
        final int status = conn.getResponseCode();
        if (range != null) {
            range.status = status;
            range.contentType = conn.getContentType();
            range.contentLength = conn.getContentLengthLong();
            range.contentRange = conn.getHeaderField("Content-Range");
            range.acceptRanges = conn.getHeaderField("Accept-Ranges");
        }
        InputStream in = null;
        try {
            in = status >= 400 ? conn.getErrorStream() : conn.getInputStream();
            if (in == null) return;
            byte[] buffer = new byte[64 * 1024];
            int read;
            while ((read = in.read(buffer)) > 0) {
                out.write(buffer, 0, read);
                out.flush();
            }
        } finally {
            if (in != null) try { in.close(); } catch (IOException ignored) { }
            conn.disconnect();
        }
    }

    /** 给 pipe 回填上游状态，供 PluginServer 写响应头 */
    static final class RangeInfo {
        int status = 200;
        String contentType;
        long contentLength = -1;
        String contentRange;
        String acceptRanges;
    }

    static final class HttpError extends IOException {
        final int code;
        final String body;

        HttpError(int code, String body) {
            super("HTTP " + code);
            this.code = code;
            this.body = body;
        }
    }

    static Map<String, String> headers(String... pairs) {
        Map<String, String> map = new LinkedHashMap<>();
        for (int i = 0; i + 1 < pairs.length; i += 2) map.put(pairs[i], pairs[i + 1]);
        return map;
    }

    private static String readAll(InputStream in) throws IOException {
        ByteArrayOutputStream buffer = new ByteArrayOutputStream();
        byte[] chunk = new byte[8192];
        int read;
        while ((read = in.read(chunk)) > 0) buffer.write(chunk, 0, read);
        in.close();
        return new String(buffer.toByteArray(), "UTF-8");
    }

    private static void sleep(long ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    static void log(String message) {
        Log.i(TAG, message);
    }
}
