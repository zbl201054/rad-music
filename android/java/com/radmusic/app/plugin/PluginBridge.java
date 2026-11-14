package com.radmusic.app.plugin;

import android.content.Context;
import android.util.Log;

import java.util.ArrayList;
import java.util.List;

/**
 * 内置音源插件的生命周期。
 *
 * Android 上没有 Python 运行时，所以把桌面端的 Python 插件换成本进程内的两个 HTTP 服务：
 *   8849 → bilibili
 *   8848 → netease
 * C++ 服务端仍然按原协议（plugin.json 里的 endpoint）去 ping / 调 op，无感知。
 */
public final class PluginBridge {

    public static final int NETEASE_PORT = 8848;
    public static final int BILIBILI_PORT = 8849;
    public static final int RECOMMEND_PORT = 8850;

    private static final String TAG = "RadMusicPlugin";
    private static final List<PluginServer> SERVERS = new ArrayList<>();

    private PluginBridge() {}

    /** 启动内置插件（幂等；端口已被占用时只记日志，不影响主流程） */
    public static synchronized void start(Context context) {
        if (!SERVERS.isEmpty()) return;

        final java.io.File dir = new java.io.File(context.getApplicationContext().getFilesDir(), "plugin-cookies");
        //noinspection ResultOfMethodCallIgnored
        dir.mkdirs();
        PluginServer bili = new PluginServer(BILIBILI_PORT,
                new BiliSource(BILIBILI_PORT, new java.io.File(dir, "bilibili.txt")));
        PluginServer netease = new PluginServer(NETEASE_PORT,
                new NeteaseSource(NETEASE_PORT, new java.io.File(dir, "netease.txt")));
        PluginServer recommend = new PluginServer(RECOMMEND_PORT,
                new RecommendSource(RECOMMEND_PORT, new java.io.File(dir, "recommend.json")));

        for (PluginServer server : new PluginServer[]{bili, netease, recommend}) {
            try {
                server.start();
                SERVERS.add(server);
            } catch (Exception e) {
                Log.w(TAG, "内置插件启动失败（端口 " + server.port() + "）：" + e.getMessage());
            }
        }
    }

    public static synchronized void stop() {
        for (PluginServer server : SERVERS) server.stop();
        SERVERS.clear();
    }

    public static synchronized boolean running() {
        return !SERVERS.isEmpty();
    }
}
