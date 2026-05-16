#include <iostream>
#include <csignal>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <vector>
#include <thread>
#include <memory>
#include <algorithm>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#if defined(__has_include)
#  if __has_include(<SDL2/SDL.h>)
#    include <SDL2/SDL.h>
#    define HAVE_SDL 1
#  else
#    define HAVE_SDL 0
#  endif
#else
#  define HAVE_SDL 0
#endif

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
#include <otcsdk/common/ImageInfo.h>

using namespace optris;

static std::atomic<bool> exit_loop(false);

void sigint_handler(int) {
    exit_loop = true;
}

class ThermalToNDI : public IRImagerClient {
public:
    ThermalToNDI()
        : ndi_sender_(nullptr),
          width_(0), height_(0),
          frame_count_(0),
          pending_frames_(0),
          preview_enabled_(false),
          current_palette_index_(0)
    {}

    ~ThermalToNDI() noexcept override
    {
        if (ndi_sender_) {
            NDIlib_send_destroy(ndi_sender_);
        }
    }

    bool setPreviewEnabled(bool enabled)
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        preview_enabled_ = enabled;
        return preview_enabled_;
    }

    bool cyclePalette()
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (available_palettes_.empty() || !image_builder_) {
            return false;
        }

        current_palette_index_ = (current_palette_index_ + 1) % available_palettes_.size();
        return applyPaletteUnlocked(available_palettes_[current_palette_index_]);
    }

    std::string currentPaletteName() const
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (available_palettes_.empty()) {
            return "unknown";
        }
        return available_palettes_[current_palette_index_];
    }

    bool renderPreview(std::ostream& out)
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!preview_enabled_ || rgba_buffer_.empty() || width_ == 0 || height_ == 0) {
            return false;
        }

        const int preview_width = 48;
        const int preview_height = 24;
        static const char* shade = " .:-=+*#%@";

        out << "\033[H\033[J";
        out << "Thermal preview | palette=" << currentPaletteNameUnlocked()
            << " | frames=" << frame_count_ << "\n";

        for (int y = 0; y < preview_height; ++y) {
            const int src_y = y * height_ / preview_height;
            for (int x = 0; x < preview_width; ++x) {
                const int src_x = x * width_ / preview_width;
                const int idx = (src_y * width_ + src_x) * 4;
                const unsigned char r = rgba_buffer_[idx + 0];
                const unsigned char g = rgba_buffer_[idx + 1];
                const unsigned char b = rgba_buffer_[idx + 2];
                const int luminance = (static_cast<int>(r) * 30 + static_cast<int>(g) * 59 + static_cast<int>(b) * 11) / 100;
                const int shade_index = luminance * 9 / 255;
                out << shade[shade_index];
            }
            out << '\n';
        }

        out << "Controls: p palette | v preview | a auto-flag | [/] interval | f force flag | q quit\n";
        return true;
    }

    // Thread-safe getter for latest RGBA buffer
    bool getLatestRGBA(std::vector<unsigned char>& out_buf, int& out_w, int& out_h) const
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (rgba_buffer_.empty() || width_ == 0 || height_ == 0) return false;
        out_buf = rgba_buffer_;
        out_w = width_;
        out_h = height_;
        return true;
    }

    bool initialize(const std::string& ndi_name)
    {
        // Create NDI sender
        NDIlib_send_create_t send_create_desc = {};
        send_create_desc.p_ndi_name = ndi_name.c_str();
        // CRITICAL: Disable clock_video to eliminate NDI-side buffering (~500ms latency reduction).
        // Frames flow at sender's pace instead of NDI's internal clock.
        send_create_desc.clock_video = false;

        ndi_sender_ = NDIlib_send_create(&send_create_desc);
        return ndi_sender_ != nullptr;
    }

    // Callback when thermal frame arrives
    void onFrame(const FrameEvent& evt) noexcept override
    {
        try {
            const ThermalFrame& thermal_frame = evt.thermalFrame;

            // First frame: initialize NDI frame descriptor and image palette builder
            if (width_ == 0) {
                width_ = thermal_frame.getWidth();
                height_ = thermal_frame.getHeight();
                rgba_buffer_.resize(width_ * height_ * 4);

                image_builder_ = std::make_unique<ImageBuilder>(ColorFormat::RGB, WidthAlignment::OneByte);
                discoverPalettesUnlocked();

                std::cout << "Thermal resolution: " << width_ << "x" << height_ << std::endl;
                std::cout << "Color palette: " << currentPaletteNameUnlocked() << " (Optris SDK)" << std::endl;
            }

            // Convert thermal data to RGBA using Optris SDK palette
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                image_builder_->setThermalFrame(thermal_frame);
                convert_thermal_to_rgbaUnlocked(rgba_buffer_.data());
            }

            // Send to NDI with proper frame timing metadata
            NDIlib_video_frame_v2_t ndi_frame = {};
            ndi_frame.xres = width_;
            ndi_frame.yres = height_;
            ndi_frame.FourCC = NDIlib_FourCC_type_RGBA;
            ndi_frame.frame_format_type = NDIlib_frame_format_type_progressive;
            ndi_frame.line_stride_in_bytes = width_ * 4;
            ndi_frame.p_data = rgba_buffer_.data();
            // Set frame timing for 32 fps camera
            ndi_frame.frame_rate_N = 32000;
            ndi_frame.frame_rate_D = 1000;
            ndi_frame.picture_aspect_ratio = (float)width_ / height_;
            ndi_frame.timecode = NDIlib_send_timecode_synthesize;  // Let NDI handle timecode

            // Drop frame if callback queue is backed up (reduces latency on slow receivers)
            if (pending_frames_ > 2) {
                pending_frames_--;
                return;  // Skip sending this frame
            }

            pending_frames_++;
            NDIlib_send_send_video_v2(ndi_sender_, &ndi_frame);

            frame_count_++;
            if (frame_count_ % 300 == 0) {
                std::cout << "Frames sent: " << frame_count_ << std::endl;
            }

        } catch (const std::exception& e) {
            std::cerr << "Error in onFrame: " << e.what() << std::endl;
        }
    }

    void onConnection(ConnectionEvent& evt) noexcept override
    {
        (void)evt;
        std::cout << "Connection state changed" << std::endl;
    }

    int getFrameCount() const { return frame_count_; }

private:
    NDIlib_send_instance_t ndi_sender_;
    int width_;
    int height_;
    int frame_count_;
    int pending_frames_;
    bool preview_enabled_;
    std::vector<std::string> available_palettes_;
    std::size_t current_palette_index_;
    std::unique_ptr<ImageBuilder> image_builder_;
    mutable std::mutex state_mutex_;
    std::vector<unsigned char> rgba_buffer_;

    /**
     * Convert thermal frame to RGBA using Optris SDK color palette
     * Expands RGB output to RGBA for NDI
     */
    void convert_thermal_to_rgbaUnlocked(unsigned char* rgba_buffer) noexcept
    {
        if (!image_builder_) return;

        // Get RGB image size (3 bytes per pixel)
        const int rgb_size = image_builder_->getImageSizeInBytes();
        std::vector<unsigned char> rgb_buffer(rgb_size);

        // Render thermal data to RGB using palette
        image_builder_->convertTemperatureToPaletteImage(rgb_buffer.data(), rgb_size);

        // Expand RGB to RGBA: copy each RGB pixel and add alpha channel
        const int pixel_count = width_ * height_;
        for (int i = 0; i < pixel_count; ++i) {
            int rgb_idx = i * 3;
            int rgba_idx = i * 4;
            rgba_buffer[rgba_idx + 0] = rgb_buffer[rgb_idx + 0];  // R
            rgba_buffer[rgba_idx + 1] = rgb_buffer[rgb_idx + 1];  // G
            rgba_buffer[rgba_idx + 2] = rgb_buffer[rgb_idx + 2];  // B
            rgba_buffer[rgba_idx + 3] = 255;                      // A
        }
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
};

class RawTerminal {
public:
    RawTerminal()
        : active_(false),
          old_flags_(-1)
    {
        if (!isatty(STDIN_FILENO)) {
            return;
        }

        if (tcgetattr(STDIN_FILENO, &old_termios_) != 0) {
            return;
        }

        termios raw = old_termios_;
        raw.c_lflag &= static_cast<unsigned long>(~(ICANON | ECHO));
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
            return;
        }

        old_flags_ = fcntl(STDIN_FILENO, F_GETFL, 0);
        if (old_flags_ != -1) {
            fcntl(STDIN_FILENO, F_SETFL, old_flags_ | O_NONBLOCK);
        }

        active_ = true;
    }

    ~RawTerminal()
    {
        if (!active_) {
            return;
        }

        tcsetattr(STDIN_FILENO, TCSANOW, &old_termios_);
        if (old_flags_ != -1) {
            fcntl(STDIN_FILENO, F_SETFL, old_flags_);
        }
    }

    bool readKey(char& key)
    {
        if (!active_) {
            return false;
        }

        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(STDIN_FILENO, &read_fds);
        timeval timeout {0, 100000};

        const int ready = select(STDIN_FILENO + 1, &read_fds, nullptr, nullptr, &timeout);
        if (ready <= 0 || !FD_ISSET(STDIN_FILENO, &read_fds)) {
            return false;
        }

        return read(STDIN_FILENO, &key, 1) == 1;
    }

private:
    bool active_;
    int old_flags_;
    termios old_termios_;
};

int main(int argc, char* argv[])
{
    try {
        // Initialize Optris SDK and device enumeration (required before camera connect).
        Sdk::init(Verbosity::Info, Verbosity::Off, argv[0]);

        // Load color palettes from SDK
        Sdk::loadPalettes();

        // Optional: scan typical LAN subnet for Ethernet cameras.
        if (argc >= 2) {
            const std::string maybe_subnet = argv[1];
            if (maybe_subnet.find('/') != std::string::npos) {
                EnumerationManager::getInstance().addEthernetDetector(maybe_subnet);
                std::cout << "Added Ethernet detector for subnet: " << maybe_subnet << std::endl;
            }
        }

        // Initialize NDI
        if (!NDIlib_initialize()) {
            std::cerr << "NDI initialization failed. Check CPU compatibility." << std::endl;
            return 1;
        }

        // Set signal handler for graceful shutdown
        signal(SIGINT, &sigint_handler);

        // Create Optris thermal camera
        std::cout << "Creating Optris camera instance..." << std::endl;
        auto camera = IRImagerFactory::getInstance().create("native");
        if (!camera) {
            std::cerr << "Failed to create IRImager instance" << std::endl;
            NDIlib_destroy();
            return 1;
        }

        // Optional serial argument: ./optris2ndi <serial> OR ./optris2ndi <subnet> <serial>
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

        // Show what the SDK can currently detect (helps debug "No suitable device").
        const auto detected = EnumerationManager::getInstance().getDetectedDevices(2);
        std::cout << "Detected Optris devices: " << detected.size() << std::endl;
        for (const auto& dev : detected) {
            std::cout << "  - serial=" << dev.getSerialNumber()
                      << ", iface=" << dev.getConnectionInterface()
                      << ", busy=" << (dev.isBusy() ? "yes" : "no") << std::endl;
        }

        // Connect to camera (serial number 0 = auto-detect first camera)
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

        // Reduce pool size to minimize buffering (trade: slight increased chance of frame drops under extreme load).
        camera->setProcessingMaxResultPoolSize(5);
        camera->setAutoFlagEnabled(true);
        camera->setFlagInterval(30.0f, 120.0f);

        // Create and register thermal-to-NDI converter
        ThermalToNDI converter;
        if (!converter.initialize("Optris Thermal Camera")) {
            std::cerr << "Failed to initialize NDI sender" << std::endl;
            camera->disconnect();
            NDIlib_destroy();
            return 1;
        }

        // Register callback
        camera->addClient(&converter);

        // Start processing loop so onFrame callbacks fire.
        if (!camera->runAsync()) {
            std::cerr << "Failed to start camera processing loop" << std::endl;
            camera->removeClient(&converter);
            camera->disconnect();
            NDIlib_destroy();
            return 1;
        }

        std::cout << "Starting thermal capture and NDI streaming..." << std::endl;
        std::cout << "Color palette: dynamic, press p to cycle" << std::endl;
        std::cout << "Controls: p palette | v preview | a auto-flag | [/] interval | f force flag | q quit" << std::endl;
        std::cout << "Optimizations: clock_video=false, frame drop on backlog, reduced pool size" << std::endl;
        std::cout << "Press Ctrl+C or q to exit" << std::endl;

        RawTerminal terminal_mode;
        bool preview_enabled = false;
        bool auto_flag_enabled = true;
        float flag_min_interval = 30.0f;
        float flag_max_interval = 120.0f;
        converter.setPreviewEnabled(preview_enabled);
        std::cout << "Auto-flag: on, interval 30s to 120s" << std::endl;

        // Try to initialize SDL for windowed preview. If it fails, fallback to ASCII preview.
#if HAVE_SDL
        bool sdl_ok = false;
        SDL_Window* window = nullptr;
        SDL_Renderer* renderer = nullptr;
        SDL_Texture* texture = nullptr;
        int tex_w = 0, tex_h = 0;
        if (SDL_Init(SDL_INIT_VIDEO) == 0) {
            sdl_ok = true;
        } else {
            std::cout << "SDL init failed, falling back to terminal preview" << std::endl;
        }
#else
    bool sdl_ok = false;
    void* window = nullptr;
    void* renderer = nullptr;
    void* texture = nullptr;
    int tex_w = 0, tex_h = 0;
    std::cout << "SDL not available at compile time, falling back to terminal preview" << std::endl;
#endif

        // Wait until user presses Ctrl+C
        while (!exit_loop) {
            // Handle SDL events if available (window keyboard will take precedence)
#if HAVE_SDL
            if (sdl_ok) {
                SDL_Event ev;
                while (SDL_PollEvent(&ev)) {
                    if (ev.type == SDL_QUIT) exit_loop = true;
                    if (ev.type == SDL_KEYDOWN) {
                        char key = 0;
                        switch (ev.key.keysym.sym) {
                            case SDLK_q: key = 'q'; break;
                            case SDLK_p: key = 'p'; break;
                            case SDLK_v: key = 'v'; break;
                            case SDLK_a: key = 'a'; break;
                            case SDLK_LEFTBRACKET: key = '['; break;
                            case SDLK_RIGHTBRACKET: key = ']'; break;
                            case SDLK_f: key = 'f'; break;
                            default: key = 0; break;
                        }
                        if (key) {
                            terminal_mode.readKey(key); // no-op, keep uniform handling
                            switch (key) {
                                case 'q': case 'Q': exit_loop = true; break;
                                case 'p': case 'P': if (converter.cyclePalette()) std::cout << "Palette changed to " << converter.currentPaletteName() << std::endl; break;
                                case 'v': case 'V': preview_enabled = !preview_enabled; converter.setPreviewEnabled(preview_enabled); std::cout << "Preview " << (preview_enabled ? "enabled" : "disabled") << std::endl; break;
                                case 'a': case 'A': auto_flag_enabled = !auto_flag_enabled; camera->setAutoFlagEnabled(auto_flag_enabled); std::cout << "Auto-flag " << (auto_flag_enabled ? "enabled" : "disabled") << std::endl; break;
                                case '[': flag_max_interval = std::max(10.0f, flag_max_interval - 10.0f); flag_min_interval = std::min(flag_min_interval, flag_max_interval - 5.0f); if (flag_min_interval < 5.0f) flag_min_interval = 5.0f; camera->setFlagInterval(flag_min_interval, flag_max_interval); std::cout << "Flag interval: " << flag_min_interval << "s to " << flag_max_interval << "s" << std::endl; break;
                                case ']': flag_max_interval = std::min(300.0f, flag_max_interval + 10.0f); flag_min_interval = std::min(flag_min_interval + 5.0f, flag_max_interval - 5.0f); camera->setFlagInterval(flag_min_interval, flag_max_interval); std::cout << "Flag interval: " << flag_min_interval << "s to " << flag_max_interval << "s" << std::endl; break;
                                case 'f': case 'F': camera->forceFlagEvent(); std::cout << "Forced flag cycle" << std::endl; break;
                            }
                        }
                    }
                }
            }
#endif

            char key = 0;
            if (!sdl_ok && terminal_mode.readKey(key)) {
                switch (key) {
                    case 'q':
                    case 'Q':
                        exit_loop = true;
                        break;
                    case 'p':
                    case 'P':
                        if (converter.cyclePalette()) {
                            std::cout << "Palette changed to " << converter.currentPaletteName() << std::endl;
                        }
                        break;
                    case 'v':
                    case 'V':
                        preview_enabled = !preview_enabled;
                        converter.setPreviewEnabled(preview_enabled);
                        std::cout << "Preview " << (preview_enabled ? "enabled" : "disabled") << std::endl;
                        break;
                    case 'a':
                    case 'A':
                        auto_flag_enabled = !auto_flag_enabled;
                        camera->setAutoFlagEnabled(auto_flag_enabled);
                        std::cout << "Auto-flag " << (auto_flag_enabled ? "enabled" : "disabled") << std::endl;
                        break;
                    case '[':
                        flag_max_interval = std::max(10.0f, flag_max_interval - 10.0f);
                        flag_min_interval = std::min(flag_min_interval, flag_max_interval - 5.0f);
                        if (flag_min_interval < 5.0f) flag_min_interval = 5.0f;
                        camera->setFlagInterval(flag_min_interval, flag_max_interval);
                        std::cout << "Flag interval: " << flag_min_interval << "s to " << flag_max_interval << "s" << std::endl;
                        break;
                    case ']':
                        flag_max_interval = std::min(300.0f, flag_max_interval + 10.0f);
                        flag_min_interval = std::min(flag_min_interval + 5.0f, flag_max_interval - 5.0f);
                        camera->setFlagInterval(flag_min_interval, flag_max_interval);
                        std::cout << "Flag interval: " << flag_min_interval << "s to " << flag_max_interval << "s" << std::endl;
                        break;
                    case 'f':
                    case 'F':
                        camera->forceFlagEvent();
                        std::cout << "Forced flag cycle" << std::endl;
                        break;
                    default:
                        break;
                }
            }

            // SDL preview: fetch latest RGBA and update texture
            if (sdl_ok && preview_enabled) {
                std::vector<unsigned char> latest;
                int lw=0, lh=0;
                if (converter.getLatestRGBA(latest, lw, lh)) {
#if HAVE_SDL
                    if (!window) {
                        tex_w = lw; tex_h = lh;
                        window = SDL_CreateWindow("Optris Thermal", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, tex_w, tex_h, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
                        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
                        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, tex_w, tex_h);
                    }
                    if (texture) {
                        SDL_UpdateTexture(texture, NULL, latest.data(), tex_w * 4);
                        SDL_RenderClear(renderer);
                        SDL_RenderCopy(renderer, texture, NULL, NULL);
                        SDL_RenderPresent(renderer);
                    }
#endif
                }
            } else if (preview_enabled) {
                converter.renderPreview(std::cout);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

#if HAVE_SDL
        if (sdl_ok) {
            if (texture) SDL_DestroyTexture(texture);
            if (renderer) SDL_DestroyRenderer(renderer);
            if (window) SDL_DestroyWindow(window);
            SDL_Quit();
        }
#endif

        // Cleanup
        std::cout << "\nShutting down..." << std::endl;
        camera->stopRunning();
        camera->removeClient(&converter);
        camera->disconnect();
        NDIlib_destroy();

        std::cout << "Total frames sent: " << converter.getFrameCount() << std::endl;
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
