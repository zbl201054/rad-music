package com.radmusic.app.plugin;

import org.json.JSONObject;

/** 一个音源插件（宿主通过 HTTP JSON-RPC 调进来，op + params → data） */
interface Source {

    /** 平台标识（bilibili / netease），用于 /health 与日志 */
    String platform();

    /** 处理一次请求；失败抛 PluginError（带宿主认识的错误码） */
    Object request(String op, JSONObject params) throws PluginError;

    /** 插件错误：code 走宿主 Readme 第 3 节的错误码表 */
    class PluginError extends Exception {
        final int code;

        PluginError(int code, String message) {
            super(message);
            this.code = code;
        }
    }
}
