# Vulkan 渲染器架构方案 (基于 libplacebo)

## 目标

使用 libplacebo 替换当前的 CPU YUV→RGB 渲染管线（sws_scale），引入 GPU 加速渲染，同时保持清晰的分层架构。

## 为什么选 libplacebo

libplacebo 是一个成熟的 GPU 加速视频渲染库（mpv 核心渲染引擎），直接提供：
- `pl_gpu` — GPU 抽象层（通过 `pl_vulkan` 对接 Vulkan）
- `pl_renderer` — 高质量视频渲染器（YUV→RGB、缩放、色彩空间转换、色调映射、去带、反交错等）
- `pl_queue` — 帧队列 + 帧时序 + 帧混合/插值
- `pl_frame_from_avframe` / `pl_map_avframe` — 直接映射 FFmpeg AVFrame 到 GPU 纹理
- `pl_swapchain` — 交换链管理，直接输出 VkImage

不需要手写任何 Vulkan 代码或 compute shader。

---

## 架构概览

```
┌─────────────────────────────────────────────────────────────┐
│                    Application Layer                         │
│  ┌─────────────────────────────────────────────────────┐    │
│  │  VulkanDisplay (QQuickItem)                          │    │
│  │  - 从 pl_tex 创建 QSGVulkanTexture                    │    │
│  │  - 显示最终图像                                        │    │
│  └─────────────────────────────────────────────────────┘    │
├─────────────────────────────────────────────────────────────┤
│                    Render Thread                             │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐   │
│  │ AVFrame 队列  │ →  │ pl_queue     │ →  │ pl_renderer  │   │
│  │ (BoundedQueue)│    │ (时间管理)    │    │ (核心渲染)   │   │
│  └──────────────┘    └──────────────┘    └──────────────┘   │
│                                                    │         │
│  ┌─────────────────────────────────────────────────▼─────┐   │
│  │  pl_gpu / pl_vulkan (libplacebo GPU 抽象层)           │   │
│  └───────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
├─────────────────────────────────────────────────────────────┤
│                    Qt 6.x + QML                              │
│  ┌─────────────────────────────────────────────────────┐    │
│  │  QVulkanInstance / VkInstance (由 Qt 管理)           │    │
│  └─────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────┘
```

### 目录结构

```
App/             入口，QML引擎启动
Player/          解码器 + 帧队列 + PlayerController（不变）
Render/
  IRenderer.hpp       抽象渲染器接口
  Vulkan/
    VulkanContext.hpp/.cpp    VkInstance → pl_vulkan → pl_gpu
    VulkanRenderer.hpp/.cpp   渲染器实现（pl_queue + pl_renderer）
UI/
  VulkanDisplay.hpp/.cpp      QQuickItem + QSGRenderNode 显示组件
Utils/           Logger
```

---

## 核心类设计

### 1. IRenderer.hpp（抽象接口，不变）

```cpp
class IRenderer {
public:
    using FrameCallback = std::function<void(VideoFrame)>;
    using DoneCallback = std::function<void()>;

    virtual ~IRenderer() = default;
    virtual void run(FrameCallback onFrame, DoneCallback onDone) = 0;
    virtual void stop() = 0;
    virtual void play() = 0;
    virtual void pause() = 0;
    virtual bool isPlaying() const = 0;
    virtual double currentTime() const = 0;
};
```

### 2. VulkanContext（Vulkan 初始化 + libplacebo 封装）

职责：封装 Vulkan 实例/设备创建，提供 `pl_gpu`。

```cpp
class VulkanContext {
public:
    static VulkanContext& instance();

    // 使用 Qt 的 QVulkanInstance 初始化
    void init(QVulkanInstance* vkInst, VkSurfaceKHR surface);
    void destroy();

    pl_gpu gpu() const;          // libplacebo GPU 抽象
    VkDevice device() const;
    VkPhysicalDevice physicalDevice() const;
    VkInstance vkInstance() const;
    pl_log plLog() const;        // libplacebo 日志
};
```

初始化流程：
```
QVulkanInstance (Qt 创建)
    → VkInstance
    → VkSurfaceKHR (来自 QQuickWindow)
    → pl_vulkan_create() → pl_gpu
```

### 3. VulkanRenderer（核心渲染器，实现 IRenderer）

职责：
- 从 BoundedQueue 接收 AVFrame
- 通过 `pl_map_avframe()` 映射 AVFrame 到 GPU
- 喂入 `pl_queue`（帧队列 + 帧混合）
- 喂入 `pl_renderer`（YUV→RGB 渲染）
- PTS 时间由 `pl_queue` 管理
- 输出到 `pl_swapchain` → 拿到 VkImage → 传给 VulkanDisplay

```cpp
class VulkanRenderer : public IRenderer {
public:
    struct Config {
        BoundedQueue<AVFramePtr>* frameQueue;
        AVStream* videoStream;  // 用于获取色彩空间等元数据
        int srcWidth, srcHeight;
        // 需要 VulkanContext 获取 pl_gpu
        // TODO: VkSurfaceKHR 创建 swapchain
    };

    explicit VulkanRenderer(const Config& config);
    ~VulkanRenderer() override;

    // IRenderer 接口
    void run(FrameCallback onFrame, DoneCallback onDone) override;
    void stop() override;
    void play() override;
    void pause() override;
    bool isPlaying() const override;
    double currentTime() const override;

private:
    void renderLoop();
    void handleFrame(AVFrame* avframe, double pts);

    pl_gpu m_gpu;            // 来自 VulkanContext
    pl_renderer m_renderer;  // libplacebo 渲染器
    pl_queue m_queue;        // libplacebo 帧队列
    pl_swapchain m_swapchain;// 交换链（输出 RGB VkImage）

    BoundedQueue<AVFramePtr>* m_frameQueue;
    // ...
};
```

### 4. frame 映射回调（AVFrame → pl_source_frame → pl_queue）

```cpp
// map 回调：将 AVFrame 上传到 GPU
bool map_frame(pl_gpu gpu, pl_tex* tex,
               const pl_source_frame* src,
               pl_frame* out_frame) {
    AVFrame* avframe = static_cast<AVFrame*>(src->frame_data);
    pl_map_avframe(gpu, out_frame, avframe);
    return true;
}

// unmap 回调
void unmap_frame(pl_gpu gpu, pl_frame* frame,
                 const pl_source_frame* src) {
    pl_unmap_avframe(gpu, frame);
}
```

### 5. VulkanDisplay（QML 显示组件）

继承 `QQuickItem`，使用 `QSGRenderNode` 或直接通过 `QSGVulkanTexture::fromNative()` 显示 VkImage。

```cpp
class VulkanDisplay : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
public:
    void presentImage(VkImage image, VkImageLayout layout,
                      int width, int height);

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override;

private:
    struct Frame {
        VkImage image;
        VkImageLayout layout;
        int width, height;
    };
    std::mutex m_mutex;
    std::optional<Frame> m_pendingFrame;  // 来自渲染线程的最新帧
};
```

---

## 数据流（完整链路）

```
Decoder Thread:
  av_read_frame → avcodec_receive_frame → AVFramePtr → BoundedQueue.push()

Render Thread:
  AVFrame <- BoundedQueue.pop()
      |
      v
  pl_frame_from_avframe(&plFrame, avFrame)   // 填充元数据
  pl_source_frame src { .pts=..., .frame_data=avFrame, .map=map_frame, .unmap=unmap_frame }
  pl_queue_push(queue, &src)                 // 喂入帧队列
      |
      v
  pl_queue_update(queue, ...)                // 获取帧混合（处理 PTS 时钟）
      |
      v
  pl_swapchain_start_frame(swapchain, &swFrame)  // 开始交换链帧
  pl_render_image_mix(renderer, mix, &swFrame, params)  // 核心渲染
  pl_swapchain_submit_frame(swapchain)        // 提交 → 得到 VkImage
      |
      v
  VkImage + layout → VulkanDisplay::presentImage()

Qt Render Thread (Scene Graph):
  VulkanDisplay::updatePaintNode()
    → QSGRenderNode::render()
    → QSGVulkanTexture::fromNative(vkImage, ...)
    → 绘制到场景图
```

---

## 线程模型

| 线程 | 任务 | 变化 |
|------|------|------|
| Qt Main | QML、信号槽、poll() | VideoDisplay → VulkanDisplay |
| Decode Thread | FFmpeg 解码 → BoundedQueue | 不变 |
| Render Thread | pl_queue(时序) + pl_renderer(渲染) + pl_swapchain(输出) | sws_scale → libplacebo |

---

## CMake 变更

```cmake
# libplacebo
set(LIBPLACEBO_DIR "$ENV{HOME}/Code/Github/libplacebo")
set(LIBPLACEBO_INCLUDE "${LIBPLACEBO_DIR}/src/include")
set(LIBPLACEBO_LIB "${LIBPLACEBO_DIR}/build/src/libplacebo.so")

# 新增到 include 目录
target_include_directories(RhineApp PRIVATE ${LIBPLACEBO_INCLUDE})

# 新增链接
target_link_libraries(RhineApp PRIVATE ${LIBPLACEBO_LIB})

# 可能还需要链接 Vulkan SDK SPIRV 库等（参照 libplacebo.pc 的 Libs.private）
```

## main.cpp 变更

```cpp
// 1. 强制 Qt Quick 使用 Vulkan 渲染后端
QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);

// 2. 初始化 libplacebo 的 VulkanContext
VulkanContext::instance().init(...);

// 3. 启动时清理
QObject::connect(&app, &QGuiApplication::aboutToQuit, []() {
    VulkanContext::instance().destroy();
});
```

---

## 实施步骤

1. **IRenderer 抽象接口** — 从现有 Renderer 提取纯虚接口
2. **VulkanContext** — 封装 VkInstance → pl_vulkan → pl_gpu
3. **VulkanRenderer** — pl_queue + pl_renderer + pl_swapchain
4. **VulkanDisplay** — QQuickItem + QSGVulkanTexture 显示
5. **CMake + main.cpp 集成** — 链接 libplacebo，强制 Qt Vulkan 后端
6. **验证对比** — CPU/GPU 渲染画面一致性，seek/暂停/播放功能完整

---

## 关键设计决策

| 决策 | 选择 | 理由 |
|------|------|------|
| 渲染库 | libplacebo | 成熟稳定(mpv核心)，零Vulkan手写，直接映射AVFrame |
| GPU 上下文 | pl_vulkan 封装 VkInstance | 屏蔽 Vulkan 细节，libplacebo 管理所有 GPU 资源 |
| 帧时序 | pl_queue | 替代手动 PTS sleep_until，自带帧混合/插值 |
| YUV→RGB | pl_renderer | 自带色彩空间转换、缩放、色调映射等高级特性 |
| Qt 集成 | QSGRenderNode + Vulkan RHI | 保持 QML UI 完整，只替换视频显示层 |
| AVFrame 上传 | pl_map_avframe 懒加载 | 按需上传，pl_queue 管理纹理缓存和释放 |