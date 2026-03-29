FROM alpine:latest

# Use Alpine for a much smaller image size (shaves hundreds of MBs compared to Ubuntu)
RUN apk add --no-cache \
    gcc \
    musl-dev \
    binutils \
    make

WORKDIR /app
CMD ["/bin/sh"]
