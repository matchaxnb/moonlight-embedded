FROM ubuntu:26.04 AS build
ARG MOONLIGHT_CMAKE_ARGS=""

# keep in order
ENV DEV_PKGS="libopus-dev libexpat1-dev libssl-dev \
    libevdev-dev libudev-dev \
    libavcodec-dev libavutil-dev \
    libsdl2-dev \
    libx11-dev \
    libvdpau-dev libva-dev libasound2-dev libpulse-dev \
    libcurl4-openssl-dev libavahi-client-dev \
    libswscale-dev libdrm-dev libgbm-dev"

RUN apt-get update \
    && apt-get -y --no-install-recommends install \
        cmake build-essential pkg-config ${DEV_PKGS}
COPY . /src
RUN cd /src && mkdir build && cd build \
    && cmake .. -DCMAKE_INSTALL_PREFIX=/target ${MOONLIGHT_CMAKE_ARGS} \
    && make && make install

FROM ubuntu:26.04

# keep in same order as DEV_PKGS - makes it easier to ship
ENV RUNTIME_PKGS="libopus0 libexpat1 libssl3t64 \
    libevdev2 libudev1 \
    libavcodec62 libavutil60 \
    libsdl2-2.0-0 \
    libx11-6 \
    libvdpau1 libva2 libasound2t64 libpulse0 \
    libcurl4t64 libavahi-client3"

RUN apt-get update \
    && apt-get -y --no-install-recommends install ${RUNTIME_PKGS}
ENV TRANSITIVE_RUNTIME_PKGS="libegl1 libgles2"
RUN apt-get update \
    && apt-get -y --no-install-recommends install ${TRANSITIVE_RUNTIME_PKGS}
COPY --from=build /target /target
RUN cp -rv /target/* /usr/ && rm -rf /target
ENTRYPOINT ["/usr/bin/moonlight"]

