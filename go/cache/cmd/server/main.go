// Command server runs the plan cache as a standalone RESP-compatible TCP
// server, so the C++ optimizer (or any Redis client) can SET/GET cached
// query plans over the network instead of re-optimizing repeated queries.
package main

import (
	"flag"
	"log"

	"sqlopt/cache"
)

func main() {
	addr := flag.String("addr", ":6380", "TCP address to listen on")
	capacity := flag.Int("capacity", 10_000, "max number of cached plans (LRU capacity)")
	flag.Parse()

	c := cache.New(*capacity)
	defer c.Close()

	srv := cache.NewServer(c)
	log.Printf("plan cache listening on %s (capacity=%d)", *addr, *capacity)
	if err := srv.ListenAndServe(*addr); err != nil {
		log.Fatal(err)
	}
}
