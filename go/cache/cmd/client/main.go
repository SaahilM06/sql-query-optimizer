// Command client is a small redis-cli-style interactive client for the
// plan cache, useful for demos and manual testing.
//
// Usage:
//
//	go run ./cmd/client -addr localhost:6380
//	127.0.0.1:6380> SET plan:123 abc
//	OK
//	127.0.0.1:6380> GET plan:123
//	"abc"
//
// A command can also be passed directly as trailing arguments for one-shot,
// non-interactive use: `go run ./cmd/client -addr localhost:6380 GET plan:123`.
package main

import (
	"bufio"
	"flag"
	"fmt"
	"net"
	"os"
	"strconv"
	"strings"
	"unicode"

	"sqlopt/cache/internal/resp"
)

func main() {
	addr := flag.String("addr", "localhost:6380", "server address")
	flag.Parse()

	conn, err := net.Dial("tcp", *addr)
	if err != nil {
		fmt.Fprintf(os.Stderr, "could not connect to %s: %v\n", *addr, err)
		os.Exit(1)
	}
	defer conn.Close()
	reader := bufio.NewReader(conn)

	if args := flag.Args(); len(args) > 0 {
		runCommand(conn, reader, args)
		return
	}

	runREPL(conn, reader, *addr)
}

func runREPL(conn net.Conn, reader *bufio.Reader, addr string) {
	fmt.Printf("Connected to %s. Type QUIT or EXIT to leave.\n", addr)
	scanner := bufio.NewScanner(os.Stdin)

	for {
		fmt.Printf("%s> ", addr)
		if !scanner.Scan() {
			fmt.Println()
			return
		}
		line := scanner.Text()
		if strings.TrimSpace(line) == "" {
			continue
		}

		args, err := tokenize(line)
		if err != nil {
			fmt.Println("(error) " + err.Error())
			continue
		}
		if len(args) == 0 {
			continue
		}
		if cmd := strings.ToUpper(args[0]); cmd == "QUIT" || cmd == "EXIT" {
			return
		}

		runCommand(conn, reader, args)
	}
}

func runCommand(conn net.Conn, reader *bufio.Reader, args []string) {
	writer := bufio.NewWriter(conn)
	if err := resp.WriteRESP(writer, resp.EncodeCommand(args...)); err != nil {
		fmt.Println("(error) write failed: " + err.Error())
		return
	}
	if err := writer.Flush(); err != nil {
		fmt.Println("(error) write failed: " + err.Error())
		return
	}

	result, err := resp.ParseRESP(reader)
	if err != nil {
		fmt.Println("(error) read failed: " + err.Error())
		return
	}
	printValue(result)
}

func printValue(v resp.RespValue) {
	switch v.Type {
	case resp.RespSimpleString:
		fmt.Println(v.String)
	case resp.RespError:
		fmt.Println("(error) " + v.String)
	case resp.RespInteger:
		fmt.Println("(integer) " + strconv.FormatInt(v.Integer, 10))
	case resp.RespBulkString:
		fmt.Printf("%q\n", string(v.Bulk))
	case resp.RespNull:
		fmt.Println("(nil)")
	case resp.RespArray:
		if len(v.Array) == 0 {
			fmt.Println("(empty array)")
			return
		}
		for i, item := range v.Array {
			fmt.Printf("%d) ", i+1)
			printValue(item)
		}
	}
}

// tokenize splits a REPL input line into command arguments, respecting
// single- and double-quoted substrings so values containing spaces (e.g.
// SET key "hello world") work as one argument.
func tokenize(line string) ([]string, error) {
	var args []string
	var current strings.Builder
	inQuotes := false
	var quoteChar rune
	hasToken := false

	for _, c := range line {
		switch {
		case inQuotes:
			if c == quoteChar {
				inQuotes = false
			} else {
				current.WriteRune(c)
			}
		case c == '"' || c == '\'':
			inQuotes = true
			quoteChar = c
			hasToken = true
		case unicode.IsSpace(c):
			if hasToken {
				args = append(args, current.String())
				current.Reset()
				hasToken = false
			}
		default:
			current.WriteRune(c)
			hasToken = true
		}
	}
	if inQuotes {
		return nil, fmt.Errorf("unterminated quote")
	}
	if hasToken {
		args = append(args, current.String())
	}
	return args, nil
}
