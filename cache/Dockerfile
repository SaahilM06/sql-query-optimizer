FROM golang:1.25 AS builder
WORKDIR /app
COPY go.mod ./
RUN go mod download 2>/dev/null || true
COPY . .
RUN CGO_ENABLED=0 go build -o /cache-server ./cmd/server

FROM gcr.io/distroless/static-debian12
COPY --from=builder /cache-server /cache-server
ENTRYPOINT ["/cache-server"]
