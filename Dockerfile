### --------------------------------------------------------------------Default docker build time arguments
ARG AWS_SDK_CPP_VERSION="1.11.279"
ARG GITHUB_OWNER="aws"
ARG GITHUB_REPOSITORY="aws-sdk-cpp"
ARG AWS_SDK_BUILD_ONLY="s3;s3control;sts"
ARG AWS_SDK_CPP_BUILD_TYPE="Release"
ARG APP_BUILD_TYPE="Release"
ARG APP_USER_NAME="appuser"
ARG APP_USER_ID="1000"
ARG APP_GROUP_NAME="appgroup"
ARG APP_GROUP_ID="1000"
ARG APP_MOUNT_VOLUME="false"
ARG AWS_ROLE_SESSION_NAME="kops_s3_acl"
### --------------------------------------------------------------------

### -------------------------------------------------------------------- Prep base image and env
FROM debian:buster AS base

RUN set -ex && \
    apt-get update && \
    apt-get install -y g++ libcurl4-openssl-dev libssl-dev uuid-dev zlib1g-dev libpulse-dev git make cmake
### --------------------------------------------------------------------

### -------------------------------------------------------------------- Get aws-cpp-sdk and build
FROM base AS aws-sdk-builder
ARG AWS_SDK_CPP_VERSION
ARG GITHUB_OWNER
ARG GITHUB_REPOSITORY
ARG AWS_SDK_CPP_BUILD_TYPE
ARG AWS_SDK_BUILD_ONLY

ENV AWS_SDK_CPP_VERSION="${AWS_SDK_CPP_VERSION}" \
    AWS_SDK_CPP_BUILD_TYPE="${AWS_SDK_CPP_BUILD_TYPE}" \
    AWS_SDK_BUILD_ONLY="${AWS_SDK_BUILD_ONLY}" \
    GITHUB_OWNER="${GITHUB_OWNER}" \
    GITHUB_REPOSITORY="$GITHUB_REPOSITORY" \
    AWS_ROLE_SESSION_NAME=${AWS_ROLE_SESSION_NAME}
ENV ZIP_FILEPATH="${GITHUB_REPOSITORY}-${AWS_SDK_CPP_VERSION}.zip"
RUN ln -sf /bin/bash /bin/sh
WORKDIR /sdk_build/
# ENV GITHUB_URL="https://github.com/${GITHUB_OWNER}/${GITHUB_REPOSITORY}/archive/${AWS_SDK_CPP_VERSION}.zip"

RUN git clone --recurse-submodules https://github.com/aws/aws-sdk-cpp && \
    cmake "${GITHUB_REPOSITORY}" -DBUILD_ONLY="${AWS_SDK_BUILD_ONLY}" -DCMAKE_BUILD_TYPE="${AWS_SDK_CPP_BUILD_TYPE}" \
    -DENABLE_TESTING=OFF && \
    make && \
    make install &&\
    rm -rf "${GITHUB_REPOSITORY}"

### --------------------------------------------------------------------

### --------------------------------------------------------------------Build application
FROM aws-sdk-builder AS build-app
ARG APP_MOUNT_VOLUME
ARG APP_BUILD_TYPE
ENV APP_BUILD_TYPE="${APP_BUILD_TYPE}"
WORKDIR /app/
COPY . .

RUN rm -rf build; cmake -S . -B build -DCMAKE_BUILD_TYPE="${APP_BUILD_TYPE}"
WORKDIR /app/build/
RUN make && if [[ "$APP_MOUNT_VOLUME" = "true" ]] ; then rm -rf /app ; fi
### --------------------------------------------------------------------

### --------------------------------------------------------------------Build app container image
FROM base AS app-runtime
ARG APP_USER_NAME
ARG APP_USER_ID
ARG APP_GROUP_ID
ARG APP_GROUP_NAME

RUN groupadd --gid "$APP_GROUP_ID" "$APP_GROUP_NAME" \
    && useradd --uid "$APP_USER_ID" --gid "${APP_GROUP_ID}" --shell /bin/bash "$APP_USER_NAME"
USER "$APP_USER_NAME"


COPY --from=build-app /usr/local/bin/. /usr/local/bin/
COPY --from=build-app /usr/local/lib/. /usr/local/lib/
# CMD [ "./kops_s3_acl" ]
### --------------------------------------------------------------------