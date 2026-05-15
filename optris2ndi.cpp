#include <iostream>
#include <csignal>
#include <atomic>
#include <chrono>
#include <cstring>
#include <vector>
#include <thread>
#include <memory>

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
          frame_count_(0)
    {}

    ~ThermalToNDI() noexcept override
    {
        if (ndi_sender_) {
            NDIlib_send_destroy(ndi_sender_);
        }
    }

    bool initialize(const std::string& ndi_name)
    {
        // Create NDI sender
        NDIlib_send_create_t send_create_desc = {};
        send_create_desc.p_ndi_name = ndi_name.c_str();
        send_create_desc.clock_video = true;

        ndi_sender_ = NDIlib_send_create(&send_create_desc);
        return ndi_sender_ != nullptr;
    }

    // Callback when thermal frame arrives
    void onFrame(const FrameEvent& evt) noexcept override
    {
        try {
            const ThermalFrame& thermal_frame = evt.thermalFrame;

            // First frame: initialize NDI frame descriptor
            if (width_ == 0) {
                width_ = thermal_frame.getWidth();
                height_ = thermal_frame.getHeight();
                rgba_buffer_.resize(width_ * height_ * 4);

                std::cout << "Thermal resolution: " << width_ << "x" << height_ << std::endl;
            }

            // Convert thermal data to RGBA
            convert_thermal_to_rgba(thermal_frame, rgba_buffer_.data());

            // Send to NDI
            NDIlib_video_frame_v2_t ndi_frame = {};
            ndi_frame.xres = width_;
            ndi_frame.yres = height_;
            ndi_frame.FourCC = NDIlib_FourCC_type_RGBA;
            ndi_frame.frame_format_type = NDIlib_frame_format_type_progressive;
            ndi_frame.line_stride_in_bytes = width_ * 4;
            ndi_frame.p_data = rgba_buffer_.data();

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
    std::vector<unsigned char> rgba_buffer_;

    /**
     * Convert thermal frame to grayscale RGBA image
     */
    void convert_thermal_to_rgba(
        const ThermalFrame& thermal_frame,
        unsigned char* rgba_buffer)
    {
        float min_temp = 20.0f;
        float max_temp = 60.0f;

        for (int y = 0; y < height_; ++y) {
            for (int x = 0; x < width_; ++x) {
                try {
                    // Get temperature at this pixel
                    float temp = thermal_frame.getTemperature(x, y);

                    // Normalize temperature to 0-255 range
                    float normalized = (temp - min_temp) / (max_temp - min_temp);
                    normalized = std::max(0.0f, std::min(1.0f, normalized));
                    unsigned char gray_value = static_cast<unsigned char>(normalized * 255.0f);

                    // Write RGBA pixel (grayscale: R=G=B=gray_value, A=255)
                    int pixel_index = (y * width_ + x) * 4;
                    rgba_buffer[pixel_index + 0] = gray_value;  // R
                    rgba_buffer[pixel_index + 1] = gray_value;  // G
                    rgba_buffer[pixel_index + 2] = gray_value;  // B
                    rgba_buffer[pixel_index + 3] = 255;         // A
                } catch (...) {
                    // Skip pixels that throw exceptions
                }
            }
        }
    }
};

int main(int argc, char* argv[])
{
    try {
        // Initialize Optris SDK and device enumeration (required before camera connect).
        Sdk::init(Verbosity::Info, Verbosity::Off, argv[0]);

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

        // Allow a larger callback result pool to reduce dropped frame risk under load.
        camera->setProcessingMaxResultPoolSize(30);

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
        std::cout << "Temperature range: 20°C to 60°C (adjust in convert_thermal_to_rgba())" << std::endl;
        std::cout << "Press Ctrl+C to exit" << std::endl;

        // Wait until user presses Ctrl+C
        while (!exit_loop) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

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
