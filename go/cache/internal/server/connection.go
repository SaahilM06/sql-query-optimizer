package server

import (
	"bufio"
	"net"

	"sqlopt/cache/internal/command"
	"sqlopt/cache/internal/resp"
)

// handleConnection owns one client connection end to end: read a RESP
// command, dispatch it, write the RESP reply, repeat until the client
// disconnects or sends something unparseable. It knows nothing about cache
// logic -- that's entirely the dispatcher's job.
func handleConnection(conn net.Conn, dispatcher *command.Dispatcher, ctx *command.Context) {
	defer conn.Close()

	reader := bufio.NewReader(conn)
	writer := bufio.NewWriter(conn)

	for {
		args, err := resp.ParseCommand(reader)
		if err != nil {
			return // disconnect, malformed frame, or a forced Close() during shutdown
		}

		result := dispatcher.Dispatch(args, ctx)

		if err := resp.WriteRESP(writer, result); err != nil {
			return
		}
		if err := writer.Flush(); err != nil {
			return
		}
	}
}
