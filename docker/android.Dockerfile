# Android build environment for mindmaze-port (Capacitor app around the web build).
#   docker build -t mindmaze-port-android -f docker/android.Dockerfile docker/
#   docker run --rm -v "$PWD:/w" -w /w mindmaze-port-android make android
FROM node:22-bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
      openjdk-17-jdk-headless curl unzip make python3 git ca-certificates \
    && rm -rf /var/lib/apt/lists/*
ENV ANDROID_HOME=/opt/android-sdk ANDROID_SDK_ROOT=/opt/android-sdk
RUN mkdir -p $ANDROID_HOME/cmdline-tools && cd /tmp \
    && curl -fsSL -o tools.zip https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip \
    && unzip -q tools.zip -d $ANDROID_HOME/cmdline-tools && mv $ANDROID_HOME/cmdline-tools/cmdline-tools $ANDROID_HOME/cmdline-tools/latest \
    && rm tools.zip \
    && yes | $ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager --licenses >/dev/null \
    && $ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager "platform-tools" "platforms;android-35" "build-tools;35.0.0" >/dev/null
ENV PATH=$PATH:$ANDROID_HOME/platform-tools:$ANDROID_HOME/cmdline-tools/latest/bin
WORKDIR /w
