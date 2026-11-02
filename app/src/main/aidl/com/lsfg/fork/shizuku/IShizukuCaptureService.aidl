package com.lsfg.fork.shizuku;

import com.lsfg.fork.shizuku.IShizukuFrameCallback;

interface IShizukuCaptureService {
    void startCapture(int targetUid, int width, int height, int maxFps, IShizukuFrameCallback callback) = 1;
    void stopCapture() = 2;
    String describeBackend() = 3;
    void destroy() = 16777114;
}
