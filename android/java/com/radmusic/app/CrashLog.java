package com.radmusic.app;

import android.content.Context;
import android.util.Log;

import java.io.File;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * 极简崩溃记录。
 *
 * 手机上崩了没法连电脑看 logcat，所以把堆栈写进私有目录，
 * 下次启动时 MainActivity 会把上一次的崩溃摘要弹出来 —— 用户截图就能反馈。
 */
final class CrashLog {

    private static final String TAG = "RadMusicCrash";
    private static final String FILE_NAME = "crash.log";
    private static final long MAX_BYTES = 256 * 1024;

    private CrashLog() {}

    static File file(Context context) {
        File dir = new File(context.getApplicationContext().getFilesDir(), "logs");
        if (!dir.exists()) {
            //noinspection ResultOfMethodCallIgnored
            dir.mkdirs();
        }
        return new File(dir, FILE_NAME);
    }

    static void write(Context context, Throwable error) {
        try {
            File target = file(context);
            if (target.length() > MAX_BYTES) {
                //noinspection ResultOfMethodCallIgnored
                target.delete();
            }
            PrintWriter writer = new PrintWriter(new FileWriter(target, true));
            writer.println("===== " + new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(new Date()));
            error.printStackTrace(writer);
            writer.println();
            writer.close();
        } catch (Throwable t) {
            Log.e(TAG, "写崩溃日志失败", t);
        }
    }

    /** 取出上次崩溃内容（没有则返回 null） */
    static String read(Context context) {
        File target = file(context);
        if (!target.isFile() || target.length() == 0) return null;
        try {
            java.io.ByteArrayOutputStream buffer = new java.io.ByteArrayOutputStream();
            java.io.InputStream in = new java.io.FileInputStream(target);
            byte[] chunk = new byte[4096];
            int read;
            while ((read = in.read(chunk)) > 0) buffer.write(chunk, 0, read);
            in.close();
            return new String(buffer.toByteArray(), "UTF-8");
        } catch (Throwable t) {
            return null;
        }
    }

    static void clear(Context context) {
        //noinspection ResultOfMethodCallIgnored
        file(context).delete();
    }

    /** 安装全局未捕获异常处理器：写盘后交回系统默认行为（让应用正常退出） */
    static void install(final Context context) {
        final Thread.UncaughtExceptionHandler previous = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler(new Thread.UncaughtExceptionHandler() {
            @Override
            public void uncaughtException(Thread thread, Throwable error) {
                write(context, error);
                if (previous != null) previous.uncaughtException(thread, error);
            }
        });
    }
}
