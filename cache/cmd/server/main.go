// Command server runs the plan cache as a standalone RESP-compatible TCP
// server.
package main

import (
	"context"
	"flag"
	"log"
	"os/signal"
	"syscall"
	"time"

	"sqlopt/cache/internal/command"
	"sqlopt/cache/internal/config"
	"sqlopt/cache/internal/metrics"
	"sqlopt/cache/internal/persistence"
	"sqlopt/cache/internal/server"
	"sqlopt/cache/internal/storage"
)

func main() {
	cfg := config.Default()

	addr := flag.String("addr", cfg.Address, "TCP address to listen on")
	shards := flag.Int("shards", cfg.NumShards, "number of storage shards")
	maxMemoryMB := flag.Int64("max-memory-mb", cfg.MaxMemoryBytes/(1024*1024), "approximate memory budget in MiB")
	snapshotPath := flag.String("snapshot-path", cfg.SnapshotPath, "path to the persistence snapshot file (empty disables persistence)")
	snapshotInterval := flag.Duration("snapshot-interval", cfg.SnapshotInterval, "how often to auto-save a snapshot (0 disables periodic auto-save)")
	shutdownGrace := flag.Duration("shutdown-grace", 5*time.Second, "how long to wait for in-flight connections to finish during shutdown")
	flag.Parse()

	cfg.Address = *addr
	cfg.NumShards = *shards
	cfg.MaxMemoryBytes = *maxMemoryMB * 1024 * 1024
	cfg.SnapshotPath = *snapshotPath
	cfg.SnapshotInterval = *snapshotInterval

	m := metrics.New()
	store := storage.New(cfg.NumShards, cfg.MaxMemoryBytes, m)

	if cfg.SnapshotPath != "" {
		if err := persistence.LoadSnapshot(store, cfg.SnapshotPath); err != nil {
			log.Printf("warning: failed to load snapshot from %s: %v", cfg.SnapshotPath, err)
		} else {
			log.Printf("loaded snapshot from %s (%d keys)", cfg.SnapshotPath, store.DBSize())
		}
	}

	cmdCtx := &command.Context{
		Store:     store,
		Metrics:   m,
		Config:    &cfg,
		StartTime: m.StartTime,
	}
	dispatcher := command.NewDispatcher()
	srv := server.New(dispatcher, cmdCtx, m)

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	workerCtx, cancelWorkers := context.WithCancel(context.Background())
	go store.RunExpirationWorker(workerCtx, cfg.ExpirationSweepInterval)

	if cfg.SnapshotPath != "" && cfg.SnapshotInterval > 0 {
		go runSnapshotLoop(workerCtx, store, cfg.SnapshotPath, cfg.SnapshotInterval)
	}

	log.Printf("plan cache listening on %s (shards=%d, max-memory=%dMiB)", cfg.Address, cfg.NumShards, cfg.MaxMemoryBytes/(1024*1024))
	serveErr := make(chan error, 1)
	go func() { serveErr <- srv.Serve(ctx, cfg.Address) }()

	select {
	case <-ctx.Done():
		log.Println("shutdown signal received: no longer accepting new connections")
	case err := <-serveErr:
		if err != nil {
			log.Fatalf("server error: %v", err)
		}
	}

	cancelWorkers()
	srv.Shutdown(*shutdownGrace)

	if cfg.SnapshotPath != "" {
		log.Println("saving final snapshot before exit")
		if err := persistence.SaveSnapshot(store, cfg.SnapshotPath); err != nil {
			log.Printf("warning: failed to save snapshot on shutdown: %v", err)
		}
	}

	log.Println("shutdown complete")
}

func runSnapshotLoop(ctx context.Context, store *storage.ShardedStore, path string, interval time.Duration) {
	ticker := time.NewTicker(interval)
	defer ticker.Stop()
	for {
		select {
		case <-ticker.C:
			if err := persistence.SaveSnapshot(store, path); err != nil {
				log.Printf("warning: periodic snapshot save failed: %v", err)
			}
		case <-ctx.Done():
			return
		}
	}
}
