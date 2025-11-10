# DroneBridge ESP32 Build Environment
# Based on official ESP-IDF Docker image with Node.js for web interface building

FROM espressif/idf:v5.4.2

LABEL maintainer="DroneBridge ESP32"
LABEL description="Complete build environment for DroneBridge ESP32 firmware"

# Install additional dependencies
RUN apt-get update && \
    apt-get install -y \
    curl \
    git \
    wget \
    vim \
    nano \
    zip \
    unzip \
    && rm -rf /var/lib/apt/lists/*

# Install Node.js (required for building web interface)
RUN curl -fsSL https://deb.nodesource.com/setup_18.x -o nodesource_setup.sh && \
    bash nodesource_setup.sh && \
    apt-get install -y nodejs && \
    rm nodesource_setup.sh && \
    rm -rf /var/lib/apt/lists/*

# Verify Node.js installation
RUN node --version && npm --version

# Set working directory
WORKDIR /project

# The ESP-IDF environment is already set up in the base image
# idf.py and other tools are available after sourcing the environment
