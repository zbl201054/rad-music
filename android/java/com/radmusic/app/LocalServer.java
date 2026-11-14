package com.radmusic.app;

import android.content.Context;
import android.content.res.AssetManager;
import android.util.Log;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.net.URL;
import java.util.concurrent.TimeUnit;

/**
 * 本地服务进程管理。
 *
 * 服务端仍是仓库里那个零依赖的 C++17 程序（api/tools/radmusic_server.cpp），
 * 这里只是用 NDK 交叉编译成 arm64 的可执行文件，随 APK 一起放在 jniLibs 里，
 * 启动时以子进程方式拉起，监听 127.0.0.1:8765，WebView 直接访问这个地址。
 *
 * 为什么 .so 而不是 .exe：Android 只允许从 nativeLibraryDir（/data/app/…/lib/arm64，
 * 只读挂载）执行原生文件；App 私有目录在 Android 10+ 被 W^X 策略禁止 exec。
 * 因此构建脚本会把 radmusic_server 命名为 libradmusic_server.so 再放进 jniLibs，
 * 并在 AndroidManifest 里开启 extractNativeLibs="true"。
 */
final class LocalServer {

    static final int PORT = 8765;
    private static final String TAG = "RadMusicServer";

    private static Process process;
    private static File logFile;

    private LocalServer() {}

    /** 数据根：App 私有目录，卸载即清理，不需要存储权限 */
    static File dataDir(Context c) {
        return new File(c.getFilesDir(), "radmusic-data");
    }

    static File webDir(Context c) {
        return new File(c.getFilesDir(), "web");
    }

    static File logDir(Context c) {
        return new File(c.getFilesDir(), "logs");
    }

    static synchronized boolean isRunning() {
        return process != null && process.isAlive();
    }

    /** 本机端口是否已经在监听（本地服务或用户手动起的服务） */
    static boolean portReady(int timeoutMs) {
        try (Socket socket = new Socket()) {
            socket.connect(new InetSocketAddress("127.0.0.1", PORT), timeoutMs);
            return true;
        } catch (IOException e) {
            return false;
        }
    }

    /**
     * 启动原生服务端。返回前不保证已监听端口，调用方应轮询 {@link #portReady}。
     * 若端口已有服务（重复启动 / 上次没退干净）则直接复用，不再拉新进程。
     */
    static synchronized void start(Context context) throws IOException {
        if (isRunning()) return;
        if (portReady(300)) {
            Log.i(TAG, "端口 " + PORT + " 已在监听，复用现有服务");
            return;
        }

        Context app = context.getApplicationContext();
        File binary = new File(app.getApplicationInfo().nativeLibraryDir, "libradmusic_server.so");
        if (!binary.exists()) {
            throw new IOException("找不到原生服务端：" + binary.getAbsolutePath());
        }
        if (!binary.canExecute() && !binary.setExecutable(true, false)) {
            throw new IOException("原生服务端不可执行：" + binary.getAbsolutePath());
        }

        File data = dataDir(app);
        File web = webDir(app);
        File logs = logDir(app);
        if (!data.exists() && !data.mkdirs()) throw new IOException("无法创建数据目录: " + data);
        if (!logs.exists() && !logs.mkdirs()) throw new IOException("无法创建日志目录: " + logs);

        logFile = new File(logs, "server.log");

        // --no-plugin-autostart：Android 上没有 python，在线音源插件跑不起来，
        // 避免每次启动都去 spawn 一个必然失败的子进程。
        String[] command = {
                binary.getAbsolutePath(),
                "--root", data.getAbsolutePath(),
                "--web", web.getAbsolutePath(),
                "--port", String.valueOf(PORT),
                "--host", "127.0.0.1",
                "--no-plugin-autostart",
                "--no-log-console",
                "--log-level", "info",
        };

        ProcessBuilder builder = new ProcessBuilder(command);
        builder.directory(app.getFilesDir());
        builder.redirectErrorStream(true);
        builder.redirectOutput(ProcessBuilder.Redirect.appendTo(logFile));
        process = builder.start();
        Log.i(TAG, "已启动原生服务端：" + process);
    }

    /** 优雅停止：先请求 /music/shutdown，给它 2 秒写完设置与索引，超时强杀 */
    static synchronized void stop() {
        if (process == null) return;
        final Process current = process;
        process = null;

        // /music/shutdown 是网络请求。onDestroy / Service 回调都在主线程，
        // 直接发会抛 NetworkOnMainThreadException，所以丢到后台线程等它一下。
        Thread sender = new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    httpPost("/music/shutdown");
                } catch (Exception ignored) {
                    // 服务端可能已经在退出，忽略
                }
            }
        }, "server-shutdown");
        sender.start();
        try {
            sender.join(1500);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }

        try {
            if (!current.waitFor(1, TimeUnit.SECONDS)) {
                current.destroy();
                if (!current.waitFor(1, TimeUnit.SECONDS)) current.destroyForcibly();
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            current.destroyForcibly();
        }
        Log.i(TAG, "本地服务已停止");
    }

    /** 启动失败时的自检信息：进程是否已退出、日志末尾写了什么 */
    static synchronized String diagnose(Context context) {
        StringBuilder info = new StringBuilder();
        if (process != null) {
            try {
                info.append(process.isAlive() ? "服务进程：运行中但端口未就绪"
                                              : "服务进程：已退出（code=" + process.exitValue() + "）");
            } catch (IllegalThreadStateException e) {
                info.append("服务进程：运行中");
            }
        } else {
            info.append("服务进程：未启动");
        }
        info.append("\n\n—— 服务端日志末尾 ——\n").append(tailOfLog(context));
        return info.toString();
    }

    private static String tailOfLog(Context context) {
        File log = new File(logDir(context.getApplicationContext()), "server.log");
        if (!log.isFile()) return "（没有日志文件，说明进程可能根本没起来）";
        try (java.io.RandomAccessFile raf = new java.io.RandomAccessFile(log, "r")) {
            final int want = 1200;
            long length = raf.length();
            long from = Math.max(0, length - want);
            raf.seek(from);
            byte[] buffer = new byte[(int) (length - from)];
            raf.readFully(buffer);
            return new String(buffer, "UTF-8");
        } catch (IOException e) {
            return "（读取日志失败：" + e.getMessage() + "）";
        }
    }

    private static void httpPost(String path) throws IOException {
        URL url = new URL("http://127.0.0.1:" + PORT + path);
        HttpURLConnection conn = (HttpURLConnection) url.openConnection();
        try {
            conn.setRequestMethod("POST");
            conn.setConnectTimeout(800);
            conn.setReadTimeout(800);
            conn.setDoOutput(true);
            conn.getOutputStream().close();
            conn.getResponseCode();
        } finally {
            conn.disconnect();
        }
    }

    /**
     * 把 assets/web 解压到 filesDir/web。
     * 用 assets 里的 web_version.txt 判断是否需要重解压（前端改动靠这个生效），
     * 不依赖 App 版本号，调试时也能直接替换 assets 重新打包。
     */
    static void syncWebAssets(Context context) throws IOException {
        Context app = context.getApplicationContext();
        File target = webDir(app);
        String wanted = readAssetText(app.getAssets(), "web_version.txt");
        File stamp = new File(target, ".web_version");
        if (target.isDirectory() && wanted != null && wanted.equals(readFileText(stamp))) {
            return;
        }
        deleteRecursively(target);
        if (!target.mkdirs()) throw new IOException("无法创建页面目录: " + target);
        copyAssetDir(app.getAssets(), "web", target);
        if (wanted != null) writeText(stamp, wanted);
        syncPluginManifests(app);
        Log.i(TAG, "已解压前端资源到 " + target.getAbsolutePath());
    }

    /**
     * 把内置音源插件的 plugin.json 同步到 <root>/plugins/。
     * C++ 服务端启动时扫这个目录，按 endpoint 去 ping 本进程里的插件服务；
     * 清单里没有 command，所以它不会尝试拉起子进程（手机上没有 Python）。
     */
    private static void syncPluginManifests(Context app) {
        File target = new File(dataDir(app), "plugins");
        try {
            copyAssetDir(app.getAssets(), "plugins", target);
            Log.i(TAG, "插件清单已同步到 " + target.getAbsolutePath());
        } catch (IOException e) {
            Log.w(TAG, "插件清单同步失败: " + e.getMessage());
        }
    }

    // ------------------------------------------------------------ 文件工具

    private static void copyAssetDir(AssetManager assets, String path, File target) throws IOException {
        String[] children = assets.list(path);
        if (children == null || children.length == 0) {
            copyAssetFile(assets, path, target);
            return;
        }
        if (!target.exists() && !target.mkdirs()) throw new IOException("mkdirs 失败: " + target);
        for (String child : children) {
            copyAssetDir(assets, path + "/" + child, new File(target, child));
        }
    }

    private static void copyAssetFile(AssetManager assets, String path, File target) throws IOException {
        File parent = target.getParentFile();
        if (parent != null && !parent.exists() && !parent.mkdirs()) throw new IOException("mkdirs 失败: " + parent);
        try (InputStream in = new BufferedInputStream(assets.open(path));
             OutputStream out = new BufferedOutputStream(new FileOutputStream(target))) {
            byte[] buffer = new byte[64 * 1024];
            int read;
            while ((read = in.read(buffer)) > 0) {
                out.write(buffer, 0, read);
            }
        }
    }

    private static String readAssetText(AssetManager assets, String path) {
        try (InputStream in = assets.open(path)) {
            return new String(readAll(in), "UTF-8").trim();
        } catch (IOException e) {
            return null;
        }
    }

    private static String readFileText(File file) {
        if (!file.isFile()) return null;
        try (InputStream in = new java.io.FileInputStream(file)) {
            return new String(readAll(in), "UTF-8").trim();
        } catch (IOException e) {
            return null;
        }
    }

    private static void writeText(File file, String text) throws IOException {
        try (OutputStream out = new FileOutputStream(file)) {
            out.write(text.getBytes("UTF-8"));
        }
    }

    private static byte[] readAll(InputStream in) throws IOException {
        java.io.ByteArrayOutputStream buffer = new java.io.ByteArrayOutputStream();
        byte[] chunk = new byte[8192];
        int read;
        while ((read = in.read(chunk)) > 0) buffer.write(chunk, 0, read);
        return buffer.toByteArray();
    }

    private static void deleteRecursively(File file) {
        if (file == null || !file.exists()) return;
        if (file.isDirectory()) {
            File[] children = file.listFiles();
            if (children != null) {
                for (File child : children) deleteRecursively(child);
            }
        }
        //noinspection ResultOfMethodCallIgnored
        file.delete();
    }
}
