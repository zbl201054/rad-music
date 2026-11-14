package com.radmusic.app;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.graphics.drawable.Icon;
import android.os.Build;
import android.os.IBinder;
import android.util.Log;

/**
 * 前台服务：保证 App 退到后台（或锁屏）时本地服务进程不被系统回收，
 * 同时常驻一条通知，点击回到应用。
 */
public class ServerService extends Service {

    static final String CHANNEL_ID = "radmusic_server";
    static final int NOTIFICATION_ID = 1001;
    static final String ACTION_START = "com.radmusic.app.START";
    static final String ACTION_STOP = "com.radmusic.app.STOP";

    private static final String TAG = "RadMusicService";

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && ACTION_STOP.equals(intent.getAction())) {
            LocalServer.stop();
            stopForeground(true);
            stopSelf();
            return START_NOT_STICKY;
        }

        try {
            // 前台服务通知在某些 ROM / 缺少通知权限时会抛异常，
            // 这一句抛出去会让整个 App 直接闪退 —— 必须兜住。
            startForeground(NOTIFICATION_ID, buildNotification());
        } catch (Throwable t) {
            Log.e(TAG, "startForeground 失败，改用极简通知重试", t);
            CrashLog.write(this, t);
            // 有些 ROM 对通知自定义图标 / Action 挑剔，去掉它们再试一次；
            // 仍失败就继续（普通服务也能跑，只是没有通知栏入口）。
            try {
                Notification.Builder simple = Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
                        ? new Notification.Builder(this, CHANNEL_ID)
                        : new Notification.Builder(this);
                simple.setContentTitle(getString(R.string.notification_title))
                        .setSmallIcon(R.mipmap.ic_launcher)
                        .setOngoing(true);
                startForeground(NOTIFICATION_ID, simple.build());
            } catch (Throwable again) {
                Log.e(TAG, "极简通知也失败", again);
                CrashLog.write(this, again);
            }
        }

        // 解压前端资源、拉起原生进程都是耗时 I/O，放到后台线程做，
        // onStartCommand 立刻返回（主线程只负责 startForeground）。
        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    // 先把内置音源（Java 实现的 bilibili / netease 插件）起来：
                    // 服务端启动后会去 ping 这两个端口，晚起会被当成「插件未就绪」。
                    com.radmusic.app.plugin.PluginBridge.start(ServerService.this);
                    LocalServer.syncWebAssets(ServerService.this);
                    LocalServer.start(ServerService.this);
                } catch (Throwable e) {
                    Log.e(TAG, "本地服务启动失败", e);
                    CrashLog.write(ServerService.this, e);
                    // 启动失败就不必常驻通知了，交给 Activity 显示错误页
                    stopForeground(true);
                    stopSelf();
                }
            }
        }, "server-boot").start();
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        LocalServer.stop();
        com.radmusic.app.plugin.PluginBridge.stop();
        super.onDestroy();
    }

    private Notification buildNotification() {
        NotificationManager manager = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel channel = new NotificationChannel(
                    CHANNEL_ID, getString(R.string.channel_server), NotificationManager.IMPORTANCE_LOW);
            channel.setDescription(getString(R.string.channel_server_desc));
            channel.setShowBadge(false);
            manager.createNotificationChannel(channel);
        }

        Intent open = new Intent(this, MainActivity.class);
        open.setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP);
        PendingIntent contentIntent = PendingIntent.getActivity(
                this, 0, open, pendingIntentFlags());

        Intent switchIntent = new Intent(this, MainActivity.class);
        switchIntent.setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP);
        switchIntent.putExtra(MainActivity.EXTRA_SWITCH_SERVER, true);
        PendingIntent switchPending = PendingIntent.getActivity(
                this, 1, switchIntent, pendingIntentFlags());

        Notification.Builder builder = Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
                ? new Notification.Builder(this, CHANNEL_ID)
                : new Notification.Builder(this);

        builder.setContentTitle(getString(R.string.notification_title))
                .setContentText(getString(R.string.notification_text))
                .setSmallIcon(R.drawable.ic_stat_music)
                .setContentIntent(contentIntent)
                .setOngoing(true)
                .setShowWhen(false);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
            builder.addAction(new Notification.Action.Builder(
                            Icon.createWithResource(this, R.drawable.ic_stat_music),
                            getString(R.string.action_switch_server), switchPending)
                    .build());
        }
        return builder.build();
    }

    private static int pendingIntentFlags() {
        int flags = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) flags |= PendingIntent.FLAG_IMMUTABLE;
        return flags;
    }
}
