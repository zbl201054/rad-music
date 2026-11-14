package com.radmusic.app;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.app.DownloadManager;
import android.content.ActivityNotFoundException;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.graphics.Color;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.webkit.CookieManager;
import android.webkit.DownloadListener;
import android.webkit.JavascriptInterface;
import android.webkit.ValueCallback;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.Toast;

import java.util.ArrayList;
import java.util.List;

/**
 * Rad Music Android 入口。
 *
 * 界面仍然是仓库里的 web/（同一套 HTML/JS/CSS，不做第二套 UI），
 * 用 WebView 直接加载本机 127.0.0.1:8765 —— 也就是把「浏览器」这一层换成应用内的内核，
 * 用户看到的是一个独立 App，不需要装浏览器、不需要手动开网页。
 *
 * 两种模式（一体两用）：
 *   local  —— App 内置交叉编译的 C++ 服务端，播放手机本地曲库（在线音源不可用，见 LocalServer）
 *   remote —— 连到同一局域网里电脑上跑的服务，功能与桌面端完全一致（网易云 / B站 都在）
 */
public class MainActivity extends Activity {

    static final String EXTRA_SWITCH_SERVER = "switch_server";
    private static final String PREFS = "radmusic";
    private static final String KEY_MODE = "mode";            // local | remote
    private static final String KEY_REMOTE_URL = "remote_url";

    private static final int REQ_FILE = 1001;
    private static final int REQ_PERMISSIONS = 1002;

    private static final String TAG = "RadMusicMain";

    private WebView web;
    private ValueCallback<Uri[]> fileCallback;
    private SharedPreferences prefs;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private long lastBackPressed = 0L;
    private boolean booting = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        CrashLog.install(getApplicationContext());
        prefs = getSharedPreferences(PREFS, MODE_PRIVATE);

        Window window = getWindow();
        window.setStatusBarColor(Color.parseColor("#0F0F12"));
        window.setNavigationBarColor(Color.parseColor("#0F0F12"));

        web = new WebView(this);
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.parseColor("#0F0F12"));
        root.addView(web, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        setContentView(root);

        configureWebView();

        final boolean wantSwitch = getIntent() != null && getIntent().getBooleanExtra(EXTRA_SWITCH_SERVER, false);
        if (!showLastCrashIfAny()) {
            startFlow(wantSwitch);
        }
        requestRuntimePermissions();
    }

    /** 上次崩过就先让用户看到原因（截图就能反馈），关闭后再走正常流程 */
    private boolean showLastCrashIfAny() {
        // 如果是「装新版本之前」留下的记录，直接丢掉：
        // 否则用户更新后一打开就看到旧崩溃，会以为新版本还在崩。
        final java.io.File log = CrashLog.file(this);
        if (log.isFile() && log.length() > 0) {
            try {
                final long updated = getPackageManager().getPackageInfo(getPackageName(), 0).lastUpdateTime;
                if (log.lastModified() < updated) {
                    CrashLog.clear(this);
                    return false;
                }
            } catch (Exception ignored) {
                // 拿不到安装时间就照常显示
            }
        }
        final String crash = CrashLog.read(this);
        if (crash == null || crash.trim().isEmpty()) return false;
        final String tail = crash.length() > 1600 ? crash.substring(crash.length() - 1600) : crash;
        new AlertDialog.Builder(this)
                .setTitle(R.string.crash_title)
                .setMessage("（如果已经更新过 App，这条记录可能来自旧版本，点「知道了」即可\n"
                        + "清掉；若仍反复出现，把下面内容截图反馈。）\n\n" + tail)
                .setPositiveButton(R.string.crash_dismiss, new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        CrashLog.clear(MainActivity.this);
                        final boolean wantSwitch = getIntent() != null
                                && getIntent().getBooleanExtra(EXTRA_SWITCH_SERVER, false);
                        startFlow(wantSwitch);
                    }
                })
                .setCancelable(false)
                .show();
        return true;
    }

    private void startFlow(boolean wantSwitch) {
        if (wantSwitch || !prefs.contains(KEY_MODE)) {
            showStartupTip();
        } else {
            boot();
        }
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        if (intent != null && intent.getBooleanExtra(EXTRA_SWITCH_SERVER, false)) {
            showSwitchDialog();
        }
    }

    // ------------------------------------------------------------ WebView 配置

    private void configureWebView() {
        WebSettings settings = web.getSettings();
        settings.setJavaScriptEnabled(true);
        settings.setDomStorageEnabled(true);            // 前端大量使用 localStorage
        settings.setDatabaseEnabled(true);
        settings.setMediaPlaybackRequiresUserGesture(false);   // 播放页自动续播
        settings.setMixedContentMode(WebSettings.MIXED_CONTENT_ALWAYS_ALLOW);
        settings.setAllowFileAccess(false);
        settings.setAllowContentAccess(true);
        settings.setSupportZoom(false);
        settings.setBuiltInZoomControls(false);
        settings.setDisplayZoomControls(false);
        settings.setTextZoom(100);
        settings.setLoadWithOverviewMode(false);
        settings.setUseWideViewPort(false);             // 用设备宽度，不模拟 980px 视口
        settings.setCacheMode(WebSettings.LOAD_DEFAULT);
        settings.setUserAgentString(settings.getUserAgentString() + " RadMusicApp/1.0");

        CookieManager.getInstance().setAcceptCookie(true);

        web.setBackgroundColor(Color.parseColor("#0F0F12"));
        web.setOverScrollMode(View.OVER_SCROLL_NEVER);

        web.setWebViewClient(new WebViewClient() {
            @Override
            public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
                Uri uri = request.getUrl();
                String scheme = uri.getScheme();
                if ("http".equals(scheme) || "https".equals(scheme)) {
                    String host = uri.getHost();
                    // 只拦外部链接（曲目详情里可能有人贴链接），本机与局域网服务留在 WebView 里
                    if (host != null && !host.equals("127.0.0.1") && !host.equals("localhost")
                            && !host.equals(remoteHost())) {
                        try {
                            startActivity(new Intent(Intent.ACTION_VIEW, uri));
                            return true;
                        } catch (ActivityNotFoundException ignored) {
                            return false;
                        }
                    }
                }
                return false;
            }

            @Override
            public void onReceivedError(WebView view, WebResourceRequest request, WebResourceError error) {
                if (request.isForMainFrame()) {
                    showErrorPage(getString(R.string.err_load_failed, String.valueOf(error.getDescription())));
                }
            }

            @Override
            public void onPageFinished(WebView view, String url) {
                booting = false;
            }
        });

        web.setWebChromeClient(new WebChromeClient() {
            @Override
            public boolean onShowFileChooser(WebView view, ValueCallback<Uri[]> callback,
                                             FileChooserParams params) {
                if (fileCallback != null) fileCallback.onReceiveValue(null);
                fileCallback = callback;
                try {
                    Intent intent = params.createIntent();
                    intent.addCategory(Intent.CATEGORY_OPENABLE);
                    startActivityForResult(intent, REQ_FILE);
                } catch (ActivityNotFoundException e) {
                    // 部分 ROM 的 accept 过滤过严（.rdm / .rsk 之类自定义扩展），退回任意文件
                    Intent fallback = new Intent(Intent.ACTION_GET_CONTENT);
                    fallback.addCategory(Intent.CATEGORY_OPENABLE);
                    fallback.setType("*/*");
                    try {
                        startActivityForResult(Intent.createChooser(fallback, getString(R.string.pick_file)), REQ_FILE);
                    } catch (ActivityNotFoundException ignored) {
                        fileCallback = null;
                        Toast.makeText(MainActivity.this, R.string.no_file_picker, Toast.LENGTH_SHORT).show();
                        return false;
                    }
                }
                return true;
            }
        });

        web.addJavascriptInterface(new NativeBridge(), "RadMusicNative");

        // 导出音频包 / 歌单文件时走系统下载器，落到「下载」目录
        web.setDownloadListener(new DownloadListener() {
            @Override
            public void onDownloadStart(String url, String userAgent, String contentDisposition,
                                        String mimeType, long contentLength) {
                try {
                    DownloadManager.Request request = new DownloadManager.Request(Uri.parse(url));
                    request.addRequestHeader("User-Agent", userAgent);
                    request.setNotificationVisibility(
                            DownloadManager.Request.VISIBILITY_VISIBLE_NOTIFY_COMPLETED);
                    String name = guessFileName(contentDisposition, url);
                    request.setDestinationInExternalPublicDir(Environment.DIRECTORY_DOWNLOADS, name);
                    DownloadManager manager = (DownloadManager) getSystemService(Context.DOWNLOAD_SERVICE);
                    if (manager != null) manager.enqueue(request);
                    Toast.makeText(MainActivity.this, getString(R.string.download_started, name),
                            Toast.LENGTH_SHORT).show();
                } catch (Exception e) {
                    Log.e(TAG, "下载失败", e);
                    Toast.makeText(MainActivity.this, R.string.download_failed, Toast.LENGTH_SHORT).show();
                }
            }
        });
    }

    private String guessFileName(String contentDisposition, String url) {
        String fallback = "radmusic-" + System.currentTimeMillis();
        if (contentDisposition != null) {
            String[] parts = contentDisposition.split(";");
            for (String part : parts) {
                String piece = part.trim();
                if (piece.startsWith("filename*=")) {
                    String value = piece.substring("filename*=".length());
                    int quote = value.indexOf("''");
                    if (quote >= 0) value = value.substring(quote + 2);
                    try {
                        return java.net.URLDecoder.decode(value, "UTF-8");
                    } catch (Exception ignored) {
                        return value;
                    }
                }
                if (piece.startsWith("filename=")) {
                    String value = piece.substring("filename=".length()).replace("\"", "");
                    if (!value.isEmpty()) return value;
                }
            }
        }
        int slash = url.lastIndexOf('/');
        if (slash >= 0 && slash + 1 < url.length()) {
            String tail = url.substring(slash + 1);
            if (tail.contains(".")) return tail;
        }
        return fallback;
    }

    // ------------------------------------------------------------ 启动流程

    private void showStartupTip() {
        showLoadingPage();
        new AlertDialog.Builder(this)
                .setTitle(R.string.choose_mode_title)
                .setMessage(R.string.choose_mode_message)
                .setPositiveButton(R.string.mode_local, new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        prefs.edit().putString(KEY_MODE, "local").apply();
                        boot();
                    }
                })
                .setNegativeButton(R.string.mode_remote, new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        askRemoteAddress();
                    }
                })
                .setCancelable(false)
                .show();
    }

    private void showSwitchDialog() {
        String mode = prefs.getString(KEY_MODE, "local");
        String[] items = {
                getString(R.string.mode_local) + ("local".equals(mode) ? "  ✓" : ""),
                getString(R.string.mode_remote) + ("remote".equals(mode) ? "  ✓" : ""),
        };
        new AlertDialog.Builder(this)
                .setTitle(R.string.action_switch_server)
                .setItems(items, new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        if (which == 0) {
                            prefs.edit().putString(KEY_MODE, "local").apply();
                            rebootWebView();
                        } else {
                            askRemoteAddress();
                        }
                    }
                })
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    private void askRemoteAddress() {
        EditText input = new EditText(this);
        input.setHint(R.string.remote_hint);
        input.setText(prefs.getString(KEY_REMOTE_URL, ""));
        input.setSingleLine(true);
        new AlertDialog.Builder(this)
                .setTitle(R.string.remote_title)
                .setMessage(R.string.remote_message)
                .setView(input)
                .setPositiveButton(android.R.string.ok, new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        String url = normalizeUrl(input.getText().toString());
                        if (url == null) {
                            Toast.makeText(MainActivity.this, R.string.remote_invalid, Toast.LENGTH_SHORT).show();
                            return;
                        }
                        prefs.edit().putString(KEY_MODE, "remote").putString(KEY_REMOTE_URL, url).apply();
                        rebootWebView();
                    }
                })
                .setNegativeButton(android.R.string.cancel, new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        if (!prefs.contains(KEY_MODE)) showStartupTip();
                    }
                })
                .show();
    }

    private String normalizeUrl(String input) {
        String text = input == null ? "" : input.trim();
        if (text.isEmpty()) return null;
        if (!text.startsWith("http://") && !text.startsWith("https://")) text = "http://" + text;
        int schemeEnd = text.indexOf("://") + 3;
        if (text.indexOf(':', schemeEnd) < 0) {
            // 没写端口：补默认 8765（https 除外，443 是默认值）
            text = text.endsWith("/") ? text.substring(0, text.length() - 1) : text;
            if (!text.startsWith("https://")) text = text + ":" + LocalServer.PORT;
        }
        if (!text.endsWith("/")) text = text + "/";
        return text;
    }

    private String remoteHost() {
        String url = prefs.getString(KEY_REMOTE_URL, "");
        try {
            return Uri.parse(url).getHost();
        } catch (Exception e) {
            return "";
        }
    }

    private void rebootWebView() {
        if ("local".equals(prefs.getString(KEY_MODE, "local"))) {
            boot();
        } else {
            String url = prefs.getString(KEY_REMOTE_URL, "");
            if (url.isEmpty()) {
                showSwitchDialog();
                return;
            }
            showLoadingPage();
            web.loadUrl(url);
        }
    }

    private void boot() {
        if (booting) return;
        booting = true;

        String mode = prefs.getString(KEY_MODE, "local");
        if ("remote".equals(mode)) {
            String url = prefs.getString(KEY_REMOTE_URL, "");
            if (url.isEmpty()) {
                showSwitchDialog();
                booting = false;
                return;
            }
            showLoadingPage();
            web.loadUrl(url);
            return;
        }

        showLoadingPage();
        startServerService();

        final long deadline = System.currentTimeMillis() + 25_000;
        waitForPort(deadline);
    }

    /**
     * 等本地服务端口就绪。
     * 注意：socket 探测是网络操作，**必须**放在后台线程 ——
     * 在主线程里 connect 会抛 NetworkOnMainThreadException 直接崩掉。
     */
    private void waitForPort(final long deadline) {
        new Thread(new Runnable() {
            @Override
            public void run() {
                while (System.currentTimeMillis() < deadline) {
                    if (LocalServer.portReady(300)) {
                        handler.post(new Runnable() {
                            @Override
                            public void run() {
                                web.loadUrl("http://127.0.0.1:" + LocalServer.PORT + "/");
                            }
                        });
                        return;
                    }
                    try {
                        Thread.sleep(200);
                    } catch (InterruptedException e) {
                        return;
                    }
                }
                handler.post(new Runnable() {
                    @Override
                    public void run() {
                        // 把进程状态与日志末尾一起显示，真机上排查不用连电脑
                        showErrorPage(getString(R.string.err_server_timeout) + "\n\n"
                                + LocalServer.diagnose(MainActivity.this));
                    }
                });
            }
        }, "port-wait").start();
    }

    private void startServerService() {
        Intent intent = new Intent(this, ServerService.class);
        intent.setAction(ServerService.ACTION_START);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            startForegroundService(intent);
        } else {
            startService(intent);
        }
    }

    private void stopServerService() {
        Intent intent = new Intent(this, ServerService.class);
        intent.setAction(ServerService.ACTION_STOP);
        startService(intent);
    }

    // ------------------------------------------------------------ 页面

    private void showLoadingPage() {
        web.loadDataWithBaseURL(null, "<!doctype html><html><head><meta name='viewport' "
                + "content='width=device-width,initial-scale=1'><style>"
                + "html,body{height:100%;margin:0;background:#0F0F12;color:#c9c6d6;"
                + "font-family:system-ui,-apple-system,'Noto Sans SC',sans-serif}"
                + ".wrap{height:100%;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:18px}"
                + ".dot{width:54px;height:54px;border-radius:16px;background:#9a7bff;position:relative;"
                + "animation:pulse 1.4s ease-in-out infinite}"
                + ".dot:after{content:'';position:absolute;inset:0;margin:auto;width:22px;height:22px;"
                + "border:3px solid #fff;border-radius:50%;border-top-color:transparent;"
                + "animation:spin .9s linear infinite}"
                + "@keyframes spin{to{transform:rotate(360deg)}}"
                + "@keyframes pulse{0%,100%{transform:scale(1);opacity:.9}50%{transform:scale(1.06);opacity:1}}"
                + "p{margin:0;font-size:14px;color:#8b87a0}</style></head><body><div class='wrap'>"
                + "<div class='dot'></div><p>正在启动 Rad Music…</p></div></body></html>",
                "text/html", "UTF-8", null);
    }

    private void showErrorPage(String message) {
        String escaped = message == null ? "" : message.replace("&", "&amp;")
                .replace("<", "&lt;").replace(">", "&gt;").replace("\n", "<br>");
        web.loadDataWithBaseURL(null, "<!doctype html><html><head><meta name='viewport' "
                + "content='width=device-width,initial-scale=1'><style>"
                + "html,body{height:100%;margin:0;background:#0F0F12;color:#e8e6f0;"
                + "font-family:system-ui,-apple-system,'Noto Sans SC',sans-serif}"
                + ".wrap{min-height:100%;display:flex;flex-direction:column;align-items:center;justify-content:center;"
                + "padding:28px 22px;box-sizing:border-box;text-align:center;gap:14px}"
                + "h2{font-size:18px;margin:0}p{font-size:13px;line-height:1.7;color:#9d99b3;margin:0;max-width:560px;"
                + "text-align:left;white-space:pre-wrap;word-break:break-all}"
                + "button{margin-top:8px;padding:11px 22px;border-radius:999px;border:0;background:#9a7bff;"
                + "color:#fff;font-size:14px}button.ghost{background:#26242e;color:#c9c6d6}"
                + "</style></head><body><div class='wrap'><h2>打不开界面</h2><p>" + escaped + "</p>"
                + "<button onclick=\"RadMusicNative.switchServer()\">切换服务器</button>"
                + "<button class='ghost' onclick=\"location.reload()\">重试</button>"
                + "</div></body></html>", "text/html", "UTF-8", null);
    }

    public class NativeBridge {
        @JavascriptInterface
        public void switchServer() {
            handler.post(new Runnable() {
                @Override
                public void run() {
                    showSwitchDialog();
                }
            });
        }

        @JavascriptInterface
        public String appVersion() {
            return BuildConfigVersion.NAME;
        }
    }

    // ------------------------------------------------------------ 回调

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQ_FILE && fileCallback != null) {
            Uri[] result = null;
            if (resultCode == RESULT_OK && data != null) {
                if (data.getClipData() != null) {
                    int count = data.getClipData().getItemCount();
                    List<Uri> uris = new ArrayList<>();
                    for (int i = 0; i < count; i++) uris.add(data.getClipData().getItemAt(i).getUri());
                    result = uris.toArray(new Uri[0]);
                } else if (data.getData() != null) {
                    result = new Uri[]{data.getData()};
                }
            }
            fileCallback.onReceiveValue(result);
            fileCallback = null;
        }
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_BACK) {
            if (web.canGoBack()) {
                web.goBack();
                return true;
            }
            long now = System.currentTimeMillis();
            if (now - lastBackPressed < 2000) {
                finish();
            } else {
                lastBackPressed = now;
                Toast.makeText(this, R.string.back_to_exit, Toast.LENGTH_SHORT).show();
            }
            return true;
        }
        return super.onKeyDown(keyCode, event);
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        if (web == null) return;
        // 清单里声明了 orientation|screenSize，旋转时 Activity 不重建，WebView 只是换了个
        // 尺寸；部分 ROM 上合成层不会自己作废旧帧，横屏后原竖屏的内容（曲库列表这类
        // 长文本尤其明显）会残留在屏幕上。这里强制重新布局 + 重绘，并让页面按新视口
        // 跑一次 resize，让前端重新算一次跟尺寸相关的排版。
        web.requestLayout();
        web.invalidate();
        web.post(new Runnable() {
            @Override
            public void run() {
                if (web == null) return;
                web.evaluateJavascript(
                        "(function(){window.dispatchEvent(new Event('resize'));"
                                + "document.body && document.body.getBoundingClientRect();})()", null);
            }
        });
    }

    @Override
    protected void onPause() {
        super.onPause();
        // 刻意不调用 web.onPause()：部分 ROM 上它会连带暂停 <audio>，切到后台音乐就断了。
        // 只保留定时器运行，页面动画等由 WebView 自己按可见性降频。
        web.resumeTimers();
    }

    @Override
    protected void onResume() {
        super.onResume();
        web.onResume();
        web.resumeTimers();
    }

    @Override
    protected void onDestroy() {
        handler.removeCallbacksAndMessages(null);
        if (web != null) {
            web.loadUrl("about:blank");
            web.destroy();
        }
        // 本地服务跟随应用退出（远程模式没有本地进程，stop 是空操作）
        if ("local".equals(prefs.getString(KEY_MODE, "local"))) {
            stopServerService();
        }
        super.onDestroy();
    }

    // ------------------------------------------------------------ 权限

    private void requestRuntimePermissions() {
        List<String> wanted = new ArrayList<>();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            if (checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
                wanted.add(Manifest.permission.POST_NOTIFICATIONS);
            }
            if (checkSelfPermission(Manifest.permission.READ_MEDIA_AUDIO) != PackageManager.PERMISSION_GRANTED) {
                wanted.add(Manifest.permission.READ_MEDIA_AUDIO);
            }
        } else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            if (checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) != PackageManager.PERMISSION_GRANTED) {
                wanted.add(Manifest.permission.READ_EXTERNAL_STORAGE);
            }
        }
        if (!wanted.isEmpty()) {
            requestPermissions(wanted.toArray(new String[0]), REQ_PERMISSIONS);
        }
    }
}
