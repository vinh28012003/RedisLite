# Build stage
FROM golang:1.22-alpine AS builder
WORKDIR /app
COPY controller/go.mod controller/go.sum* ./
RUN go mod download 2>/dev/null || true
COPY controller/ .
RUN CGO_ENABLED=0 GOOS=linux go build -o failover-ctl .

# Runtime stage — minimal image
FROM alpine:3.19
RUN apk add --no-cache ca-certificates
COPY --from=builder /app/failover-ctl /usr/local/bin/
ENTRYPOINT ["failover-ctl"]
