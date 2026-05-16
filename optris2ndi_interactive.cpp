#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include "display/opengl/Obvious2D.h"

// NDI headers
#include <Processing.NDI.Lib.h>

// Optris headers
#include <otcsdk/IRImagerFactory.h>
#include <otcsdk/IRImager.h>
#include <otcsdk/common/ThermalFrame.h>
#include <otcsdk/events/FrameEvent.h>
#include <otcsdk/events/ConnectionEvent.h>
#include <otcsdk/Exceptions.h>
#include <otcsdk/Sdk.h>
#include <otcsdk/enumeration/EnumerationManager.h>
#include <otcsdk/ImageBuilder.h>
#include <otcsdk/FramerateCounter.h>

using namespace optris;

static std::atomic<bool> exit_loop(false);

void sigint_handler(int)
{
    exit_loop = true;
}

class ThermalToNDIInteractive : public IRImagerClient, public Obvious2DClient {
public:
    explicit ThermalToNDIInteractive(std::shared_ptr<IRImager> camera)
        : camera_(std::move(camera)),
          ndi_sender_(nullptr),
          width_(0),
          height_(0),
          frame_count_(0),
          auto_flag_enabled_(true),
          overlay_visible_(true),
          preview_visible_(true),
          current_palette_index_(0),
          image_builder_(std::make_unique<ImageBuilder>(ColorFormat::RGB, WidthAlignment::OneByte)),
          fps_(std::make_unique<FramerateCounter>(100))
    {}

    ~ThermalToNDIInteractive() noexcept override
    {
        if (ndi_sender_) {
            NDIlib_send_destroy(ndi_sender_);
        }
    }

    bool initialize(const std::string& ndi_name)
    {
        if (!camera_) {
            return false;
        }

        width_ = camera_->getWidth();
        height_ = camera_->getHeight();
        if (width_ == 0 || height_ == 0) {
            std::cerr << "Camera dimensions are not available yet" << std::endl;
            return false;
        }

        discoverPalettesUnlocked();
        createDisplayUnlocked();
        if (!display_) {
            return false;
        }

        NDIlib_send_create_t send_create_desc = {};
        send_create_desc.p_ndi_name = ndi_name.c_str();
        send_create_desc.clock_video = false;
        ndi_sender_ = NDIlib_send_create(&send_create_desc);
        if (!ndi_sender_) {
            return false;
        }

        display_->showSerialNum(true, static_cast<int>(camera_->getSerialNumber()));
        display_->setOperationModeInfo("Optris Thermal -> NDI");
        display_->setShowHelp(true);
        display_->setShowFPS(true);
        return true;
    }

    bool render()
    {
        if (exit_loop) {
            return false;
        }

        if (!display_ || !display_->isAlive()) {
            return false;
        }

        if (!preview_visible_) {
            static const unsigned char black_pixel[3] = {0, 0, 0};
            display_->draw(black_pixel,
                           1,
                           1,
                           3,
                           "",
                           0.0);
            return true;
        }

        FrameEvent event_copy;
        unsigned int local_width = 0;
        unsigned int local_height = 0;
        std::vector<unsigned char> local_rgb;

        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (!frame_ready_) {
                return true;
            }

            event_copy = latest_event_;
            local_width = width_;
            local_height = height_;
            local_rgb = rgb_buffer_;
        }

        display_->draw(local_rgb.data(),
                       local_width,
                       local_height,
                       3,
                       toString(event_copy.meta.getFlagState()),
                       fps_->getFps());
        return true;
    }

    void onFrame(const FrameEvent& evt) noexcept override
    {
        try {
            const ThermalFrame& thermal_frame = evt.thermalFrame;
            if (thermal_frame.isEmpty()) {
                return;
            }

            std::vector<unsigned char> local_rgba;

            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                latest_event_ = evt;
                fps_->trigger();

                const unsigned int frame_width = thermal_frame.getWidth();
                const unsigned int frame_height = thermal_frame.getHeight();
                if (rgb_buffer_.empty() || rgba_buffer_.empty() || width_ != frame_width || height_ != frame_height) {
                    width_ = frame_width;
                    height_ = frame_height;
                    rgb_buffer_.resize(width_ * height_ * 3);
                    rgba_buffer_.resize(width_ * height_ * 4);
                }

                image_builder_->setThermalFrame(thermal_frame);
                convertThermalToRgbaUnlocked();
                local_rgba = rgba_buffer_;
                frame_ready_ = true;

                if (local_rgba.empty()) {
                    return;
                }
            }

            if (ndi_sender_) {
                NDIlib_video_frame_v2_t ndi_frame = {};
                ndi_frame.xres = width_;
                ndi_frame.yres = height_;
                ndi_frame.FourCC = NDIlib_FourCC_type_RGBA;
                ndi_frame.frame_format_type = NDIlib_frame_format_type_progressive;
                ndi_frame.line_stride_in_bytes = width_ * 4;
                ndi_frame.p_data = local_rgba.data();
                ndi_frame.frame_rate_N = 32000;
                ndi_frame.frame_rate_D = 1000;
                ndi_frame.picture_aspect_ratio = static_cast<float>(width_) / static_cast<float>(height_);
                ndi_frame.timecode = NDIlib_send_timecode_synthesize;
                NDIlib_send_send_video_v2(ndi_sender_, &ndi_frame);
            }

            ++frame_count_;
            if (frame_count_ == 1) {
                std::cout << "First frame received: " << width_ << "x" << height_ << std::endl;
            }
            if (frame_count_ % 300 == 0) {
                std::cout << "Frames sent: " << frame_count_ << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error in onFrame: " << e.what() << std::endl;
        }
    }

    void onConnection(ConnectionEvent& evt) noexcept override
    {
        if (evt.state == ConnectionState::Lost || evt.state == ConnectionState::Timeout) {
            camera_->disconnect();
        }
    }

    void onVideoFormatChanged(const VideoFormatEvent& evt) noexcept override
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (display_) {
            display_->resizeWindow(evt.width, evt.height);
        }
    }

    void keyboardCallback(char key) override
    {
        try {
            switch (key) {
                case 'p':
                case 'P':
                    if (cyclePaletteUnlocked()) {
                        std::cout << "Palette changed to " << currentPaletteNameUnlocked() << std::endl;
                    }
                    break;
                case 'v':
                case 'V':
                    overlay_visible_ = !overlay_visible_;
                    if (display_) {
                        display_->setShowHelp(overlay_visible_);
                        display_->setShowFPS(overlay_visible_);
                    }
                    std::cout << "Overlay " << (overlay_visible_ ? "enabled" : "disabled") << std::endl;
                    break;
                case 'n':
                case 'N':
                    preview_visible_ = !preview_visible_;
                    if (display_) {
                        display_->setShowHelp(preview_visible_ && overlay_visible_);
                        display_->setShowFPS(preview_visible_ && overlay_visible_);
                    }
                    std::cout << "Preview " << (preview_visible_ ? "enabled" : "disabled") << std::endl;
                    break;
                case 'a':
                case 'A':
                    auto_flag_enabled_ = !auto_flag_enabled_;
                    camera_->setAutoFlagEnabled(auto_flag_enabled_);
                    std::cout << "Auto-flag " << (auto_flag_enabled_ ? "enabled" : "disabled") << std::endl;
                    break;
                case '[':
                    flag_max_interval_ = std::max(10.0f, flag_max_interval_ - 10.0f);
                    flag_min_interval_ = std::min(flag_min_interval_, flag_max_interval_ - 5.0f);
                    if (flag_min_interval_ < 5.0f) {
                        flag_min_interval_ = 5.0f;
                    }
                    camera_->setFlagInterval(flag_min_interval_, flag_max_interval_);
                    std::cout << "Flag interval: " << flag_min_interval_ << "s to " << flag_max_interval_ << "s" << std::endl;
                    break;
                case ']':
                    flag_max_interval_ = std::min(300.0f, flag_max_interval_ + 10.0f);
                    flag_min_interval_ = std::min(flag_min_interval_ + 5.0f, flag_max_interval_ - 5.0f);
                    camera_->setFlagInterval(flag_min_interval_, flag_max_interval_);
                    std::cout << "Flag interval: " << flag_min_interval_ << "s to " << flag_max_interval_ << "s" << std::endl;
                    break;
                case 'f':
                case 'F':
                    camera_->forceFlagEvent();
                    std::cout << "Forced flag cycle" << std::endl;
                    break;
                case 'q':
                case 'Q':
                    exit_loop = true;
                    if (display_) {
                        display_->terminate();
                    }
                    break;
                default:
                    break;
            }
        } catch (const SDKException& ex) {
            std::cerr << "Keyboard action failed: " << ex.what() << std::endl;
        }
    }

    int getFrameCount() const
    {
        return frame_count_;
    }

private:
    void createDisplayUnlocked()
    {
        if (display_ || !camera_ || width_ == 0 || height_ == 0) {
            return;
        }

        std::stringstream title;
        title << "Optris Imager - " << camera_->getDeviceType()
              << " (S/N " << camera_->getSerialNumber() << ")";

        display_ = std::make_unique<Obvious2D>(width_, height_, title.str());
        display_->registerKeyboardClient('p', this, "Cycle Palette", Obvious2D::GREEN, Obvious2D::BLACK);
        display_->registerKeyboardClient('v', this, "Toggle Overlay", Obvious2D::GREEN, Obvious2D::BLACK);
        display_->registerKeyboardClient('n', this, "Toggle Preview", Obvious2D::GREEN, Obvious2D::BLACK);
        display_->registerKeyboardClient('a', this, "Toggle Auto Flag", Obvious2D::GREEN, Obvious2D::BLACK);
        display_->registerKeyboardClient('[', this, "Decrease Flag Interval", Obvious2D::GREEN, Obvious2D::BLACK);
        display_->registerKeyboardClient(']', this, "Increase Flag Interval", Obvious2D::GREEN, Obvious2D::BLACK);
        display_->registerKeyboardClient('f', this, "Force Flag Event", Obvious2D::GREEN, Obvious2D::BLACK);
        display_->setOperationModeInfo("Optris Thermal -> NDI");
        display_->showSerialNum(true, static_cast<int>(camera_->getSerialNumber()));
    }

    void discoverPalettesUnlocked()
    {
        available_palettes_.clear();

        static const std::vector<std::string> candidates = {
            "GrayBW",
            "Iron"
        };

        for (const auto& palette : candidates) {
            try {
                image_builder_->setPalette(palette);
                available_palettes_.push_back(palette);
            } catch (const std::exception&) {
            }
        }

        if (available_palettes_.empty()) {
            available_palettes_.push_back("GrayBW");
            image_builder_->setPalette("GrayBW");
        }

        current_palette_index_ = 0;
        image_builder_->setPalette(available_palettes_[current_palette_index_]);
    }

    bool cyclePaletteUnlocked()
    {
        if (available_palettes_.empty() || !image_builder_) {
            return false;
        }

        current_palette_index_ = (current_palette_index_ + 1) % available_palettes_.size();
        return applyPaletteUnlocked(available_palettes_[current_palette_index_]);
    }

    bool applyPaletteUnlocked(const std::string& palette)
    {
        if (!image_builder_) {
            return false;
        }

        try {
            image_builder_->setPalette(palette);
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }

    std::string currentPaletteNameUnlocked() const
    {
        if (available_palettes_.empty()) {
            return "unknown";
        }

        return available_palettes_[current_palette_index_];
    }

    void convertThermalToRgbaUnlocked()
    {
        if (!image_builder_ || rgba_buffer_.empty()) {
            return;
        }

        const int rgb_size = image_builder_->getImageSizeInBytes();
        if (rgb_size <= 0) {
            return;
        }

        rgb_buffer_.resize(static_cast<std::size_t>(rgb_size));
        image_builder_->convertTemperatureToPaletteImage(rgb_buffer_.data(), rgb_size);

        // Expand RGB to RGBA: copy each RGB pixel and add alpha channel
        const int pixel_count = static_cast<int>(width_ * height_);
        for (int i = 0; i < pixel_count; ++i) {
            const int rgb_idx = i * 3;
            const int rgba_idx = i * 4;
            rgba_buffer_[rgba_idx + 0] = rgb_buffer_[rgb_idx + 0];
            rgba_buffer_[rgba_idx + 1] = rgb_buffer_[rgb_idx + 1];
            rgba_buffer_[rgba_idx + 2] = rgb_buffer_[rgb_idx + 2];
            rgba_buffer_[rgba_idx + 3] = 255;
        }
    }

    std::shared_ptr<IRImager> camera_;
    NDIlib_send_instance_t ndi_sender_;
    unsigned int width_;
    unsigned int height_;
    int frame_count_;
    bool auto_flag_enabled_;
    bool overlay_visible_;
    bool preview_visible_;
    float flag_min_interval_ = 30.0f;
    float flag_max_interval_ = 120.0f;
    std::vector<std::string> available_palettes_;
    std::size_t current_palette_index_;
    std::unique_ptr<ImageBuilder> image_builder_;
    std::unique_ptr<Obvious2D> display_;
    std::unique_ptr<FramerateCounter> fps_;
    mutable std::mutex state_mutex_;
    FrameEvent latest_event_;
    bool frame_ready_ = false;
    std::vector<unsigned char> rgb_buffer_;
    std::vector<unsigned char> rgba_buffer_;
};

int main(int argc, char* argv[])
{
    try {
        Sdk::init(Verbosity::Info, Verbosity::Off, argv[0]);
        Sdk::loadPalettes();

        if (argc >= 2) {
            const std::string maybe_subnet = argv[1];
            if (maybe_subnet.find('/') != std::string::npos) {
                EnumerationManager::getInstance().addEthernetDetector(maybe_subnet);
                std::cout << "Added Ethernet detector for subnet: " << maybe_subnet << std::endl;
            }
        }

        if (!NDIlib_initialize()) {
            std::cerr << "NDI initialization failed. Check CPU compatibility." << std::endl;
            return 1;
        }

        signal(SIGINT, &sigint_handler);

        std::cout << "Creating Optris camera instance..." << std::endl;
        auto camera = IRImagerFactory::getInstance().create("native");
        if (!camera) {
            std::cerr << "Failed to create IRImager instance" << std::endl;
            NDIlib_destroy();
            return 1;
        }

        unsigned long serial_number = 0;
        if (argc >= 2) {
            const std::string arg1 = argv[1];
            if (arg1.find('/') == std::string::npos) {
                serial_number = std::stoul(arg1);
            }
        }
        if (argc >= 3) {
            serial_number = std::stoul(argv[2]);
        }

        const auto detected = EnumerationManager::getInstance().getDetectedDevices(2);
        std::cout << "Detected Optris devices: " << detected.size() << std::endl;
        for (const auto& dev : detected) {
            std::cout << "  - serial=" << dev.getSerialNumber()
                      << ", iface=" << dev.getConnectionInterface()
                      << ", busy=" << (dev.isBusy() ? "yes" : "no") << std::endl;
        }

        if (serial_number == 0) {
            std::cout << "Connecting to Optris camera (auto-detect)..." << std::endl;
        } else {
            std::cout << "Connecting to Optris camera serial " << serial_number << "..." << std::endl;
        }
        camera->connect(serial_number);
        if (!camera->isConnected()) {
            std::cerr << "Failed to connect to camera" << std::endl;
            NDIlib_destroy();
            return 1;
        }

        std::cout << "Connected successfully!" << std::endl;
        camera->setProcessingMaxResultPoolSize(5);
        camera->setAutoFlagEnabled(true);
        camera->setFlagInterval(30.0f, 120.0f);

        ThermalToNDIInteractive viewer(camera);
        if (!viewer.initialize("Optris Thermal Camera")) {
            std::cerr << "Failed to initialize viewer" << std::endl;
            camera->disconnect();
            NDIlib_destroy();
            return 1;
        }

        camera->addClient(&viewer);
        if (!camera->runAsync()) {
            std::cerr << "Failed to start camera processing loop" << std::endl;
            camera->removeClient(&viewer);
            camera->disconnect();
            NDIlib_destroy();
            return 1;
        }

        std::cout << "Starting thermal capture and NDI streaming..." << std::endl;
        std::cout << "Controls: p palette | v overlay | n preview | a auto-flag | [/] interval | f force flag | q quit" << std::endl;
        std::cout << "Press Ctrl+C or q to exit" << std::endl;

        while (!exit_loop && viewer.render()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        std::cout << "\nShutting down..." << std::endl;
        camera->stopRunning();
        camera->removeClient(&viewer);
        camera->disconnect();
        NDIlib_destroy();

        std::cout << "Total frames sent: " << viewer.getFrameCount() << std::endl;
        std::cout << "Done!" << std::endl;
    } catch (const SDKException& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
