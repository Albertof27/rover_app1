#include <iostream>
#include <iomanip>
#include <csignal>
#include <thread>
#include <chrono>
#include <atomic>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include "config.hpp"
#include "sensor_data.hpp"

// Socket path for communication with Python BLE server
#define SOCKET_PATH "/tmp/rover_sensor.sock"

// Global flag for shutdown
std::atomic<bool> g_running(true);

// Signal handler
void signalHandler(int signum) {
    std::cout << "\n[Main] Received signal " << signum << ", shutting down..." << std::endl;
    g_running = false;
}

// Print startup banner
void printBanner() {
    std::cout << "\n";
    std::cout << "╔═══════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║     Raspberry Pi Sensor Publisher (C++)               ║" << std::endl;
    std::cout << "║     Sends data to Python BLE Server                   ║" << std::endl;
    std::cout << "╚═══════════════════════════════════════════════════════╝" << std::endl;
    std::cout << "\n";
}

// Print configuration
void printConfig() {
    std::cout << "Configuration:" << std::endl;
    std::cout << "├─ Socket Path:     " << SOCKET_PATH << std::endl;
    std::cout << "├─ Device Name:     " << Config::DEVICE_NAME << std::endl;
    std::cout << "├─ Service UUID:    " << Config::SERVICE_UUID << std::endl;
    std::cout << "├─ Weight Char:     " << Config::WEIGHT_CHAR_UUID << std::endl;
    std::cout << "├─ Events Char:     " << Config::EVENTS_CHAR_UUID << std::endl;
    std::cout << "├─ Weight Range:    " << Config::WEIGHT_MIN << " - " << Config::WEIGHT_MAX << " lb" << std::endl;
    std::cout << "├─ Overweight At:   " << Config::WEIGHT_THRESHOLD << " lb" << std::endl;
    std::cout << "└─ Update Interval: " << Config::WEIGHT_UPDATE_INTERVAL_MS << " ms" << std::endl;
    std::cout << "\n";
}

// Print event flags in human-readable format
std::string eventFlagsToString(uint16_t flags) {
    if (flags == 0) {
        return "NONE";
    }
    
    std::string result;
    if (flags & Config::EVENT_OVERWEIGHT) {
        result += "OVERWEIGHT ";
    }
    if (flags & Config::EVENT_OUT_OF_RANGE) {
        result += "OUT_OF_RANGE ";
    }
    if (flags & Config::EVENT_LOW_BATTERY) {
        result += "LOW_BATTERY ";
    }
    if (flags & Config::EVENT_ERROR) {
        result += "ERROR ";
    }
    
    return result;
}

///////////////////////////////////////////////////////////////////////////
// Inside your main function or a dedicated thread:
void gpsListener(SensorDataManager& manager) {
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);

    if (sockfd < 0) return;

    // Set a timeout so the thread doesn't get stuck during shutdown
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));


    sockaddr_in servaddr{};
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(Config::UDP_PORT);

    bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr));

    char buffer[1024];
    while (g_running) {
        int n = recvfrom(sockfd, buffer, 1023, 0, nullptr, nullptr);
        if (n > 0) {
            buffer[n] = '\0';
        
            // Expecting format "lat,lon" from Python
            float lat, lon;
            if (sscanf(buffer, "%f,%f", &lat, &lon) == 2) {
                manager.updateUserLocation(lat, lon);
            }
            else {
                // A helpful error print just in case the format changes again
                std::cout << "\n[UDP Error] Failed to parse coordinates from: " << buffer << std::endl;
            }
        }
    }
    close(sockfd);
}

//////////////////////////////////////////////////////////////







////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////
// Listen for D-Pad motor commands from Python via UDP

//////////////////////////////////////////////////////////////








// Socket publisher class for IPC with Python BLE server
class SocketPublisher {
private:
    int serverFd_ = -1;
    int clientFd_ = -1;
    std::atomic<bool> running_{false};
    std::thread acceptThread_;
    SensorDataManager& sensorManager_;

public:
    SocketPublisher(SensorDataManager& manager) : sensorManager_(manager) {}
    
    ~SocketPublisher() {
        stop();
    }
    
    bool start() {
        // Create Unix domain socket
        serverFd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (serverFd_ < 0) {
            std::cerr << "[Socket] Failed to create socket: " << strerror(errno) << std::endl;
            return false;
        }
        
        // Remove old socket file if exists
        unlink(SOCKET_PATH);
        
        // Bind socket
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
        
        if (bind(serverFd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "[Socket] Failed to bind: " << strerror(errno) << std::endl;
            close(serverFd_);
            serverFd_ = -1;
            return false;
        }
        
        // Set socket permissions so Python can connect without sudo
        chmod(SOCKET_PATH, 0666);
        
        // Listen for connections
        if (listen(serverFd_, 1) < 0) {
            std::cerr << "[Socket] Failed to listen: " << strerror(errno) << std::endl;
            close(serverFd_);
            serverFd_ = -1;
            return false;
        }
        
        running_ = true;
        acceptThread_ = std::thread(&SocketPublisher::acceptLoop, this);
        
        std::cout << "[Socket] Server listening on " << SOCKET_PATH << std::endl;
        return true;
    }
    
    void stop() {
        running_ = false;
        
        // Close sockets to unblock accept()
        if (clientFd_ >= 0) {
            close(clientFd_);
            clientFd_ = -1;
        }
        if (serverFd_ >= 0) {
            shutdown(serverFd_, SHUT_RDWR);
            close(serverFd_);
            serverFd_ = -1;
        }
        
        if (acceptThread_.joinable()) {
            acceptThread_.join();
        }
        
        unlink(SOCKET_PATH);
        std::cout << "[Socket] Server stopped" << std::endl;
    }
    
    bool sendData() {
        if (clientFd_ < 0) {
            return false;
        }
        
        // Get binary data from sensor manager
        auto weightBytes = sensorManager_.getWeightDataBinary();
        auto eventsBytes = sensorManager_.getEventsDataBinary();
        auto bearingBytes = sensorManager_.getBearingDataBinary();
        
        // Format: "W:<4 hex bytes>,E:<2 hex bytes>\n"
        // Example: "W:0000803F,E:0000\n" (weight as float bytes, events as uint16 bytes)
        char buffer[128];
        snprintf(buffer, sizeof(buffer), 
                 "W:%02X%02X%02X%02X,E:%02X%02X,B:%02X%02X%02X%02X\n",
                 weightBytes[0], weightBytes[1], weightBytes[2], weightBytes[3],
                 eventsBytes[0], eventsBytes[1],
                bearingBytes[0], bearingBytes[1], bearingBytes[2], bearingBytes[3]);
        
        // Send to Python BLE server
        ssize_t sent = send(clientFd_, buffer, strlen(buffer), MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EPIPE || errno == ECONNRESET) {
                std::cout << "\n[Socket] Python BLE server disconnected" << std::endl;
                close(clientFd_);
                clientFd_ = -1;
            }
            return false;
        }
        
        return true;
    }
    
    bool isClientConnected() const {
        return clientFd_ >= 0;
    }

private:
    void acceptLoop() {
        while (running_) {
            if (clientFd_ >= 0) {
                // Already have a client, wait
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            
            std::cout << "[Socket] Waiting for Python BLE server to connect..." << std::endl;
            
            // Set timeout on accept using select
            fd_set readfds;
            struct timeval tv;
            FD_ZERO(&readfds);
            FD_SET(serverFd_, &readfds);
            tv.tv_sec = 1;
            tv.tv_usec = 0;
            
            int ret = select(serverFd_ + 1, &readfds, nullptr, nullptr, &tv);
            if (ret > 0 && FD_ISSET(serverFd_, &readfds)) {
                int newClient = accept(serverFd_, nullptr, nullptr);
                if (newClient >= 0) {
                    clientFd_ = newClient;
                    std::cout << "[Socket] Python BLE server connected!" << std::endl;
                }
            }
        }
    }
};

// Main function
int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    
    printBanner();
    printConfig();
    
    // Setup signal handlers for graceful shutdown
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    // Create sensor data manager
    SensorDataManager sensorManager;

    sensorManager.updatePiLocation();
    
    // Create socket publisher
    SocketPublisher socketPublisher(sensorManager);

    // 1. LAUNCH THE GPS THREAD HERE
    std::thread gpsThread(gpsListener, std::ref(sensorManager));


    //std::thread motorThread(motorCommandListener);
    
    // Start socket server
    if (!socketPublisher.start()) {
        std::cerr << "[Main] Failed to start socket server" << std::endl;
        return 1;
    }
    
    // Start sensor data generation
    std::cout << "[Main] Starting sensor data generation..." << std::endl;
    sensorManager.startDataGeneration();
    
    // Print running message
    std::cout << "\n";
    std::cout << "╔═══════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║  C++ SENSOR PUBLISHER RUNNING                         ║" << std::endl;
    std::cout << "║                                                       ║" << std::endl;
    std::cout << "║  Now start the Python BLE server in another terminal: ║" << std::endl;
    std::cout << "║    sudo python3 ble_server.py                         ║" << std::endl;
    std::cout << "║                                                       ║" << std::endl;
    std::cout << "║  Press Ctrl+C to stop                                 ║" << std::endl;
    std::cout << "╚═══════════════════════════════════════════════════════╝" << std::endl;
    std::cout << "\n";
    
    // Counter for status updates
    int updateCounter = 0;
    
    // Main loop - send data and display status
    while (g_running) {
        // Send data to Python BLE server
        bool sent = socketPublisher.sendData();
        
        // Get current sensor data for display
        WeightData weight = sensorManager.getWeightData();
        EventsData events = sensorManager.getEventsData();
        
        // Display status
        std::cout << "\r[" << std::setw(5) << updateCounter << "] "
                  << "Weight: " << std::fixed << std::setprecision(1) << std::setw(6) << weight.weight << " lb"
                  << " | Events: 0x" << std::hex << std::setw(4) << std::setfill('0') << events.eventBits 
                  << std::dec << std::setfill(' ')
                  << " (" << std::setw(12) << std::left << eventFlagsToString(events.eventBits) << std::right << ")"
                  << " | BLE: " << (sent ? "✓ sending" : "○ waiting")
                  << "    " << std::flush;
        
        updateCounter++;
        
        // Sleep for update interval
        std::this_thread::sleep_for(
            std::chrono::milliseconds(Config::WEIGHT_UPDATE_INTERVAL_MS)
        );
    }
    
    // Cleanup
    std::cout << "\n\n[Main] Shutting down..." << std::endl;
    sensorManager.stopDataGeneration();
    socketPublisher.stop();

    if (gpsThread.joinable()) {
        gpsThread.join();
    }

/*
    if (motorThread.joinable()) {
        motorThread.join();
    }
    */
    
    std::cout << "[Main] Goodbye!" << std::endl;
    return 0;
}


