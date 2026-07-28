package resp

import (
	"bufio"
	"fmt"
	"io"
	"strconv"
	"strings"
)

// Upper bounds on attacker-controlled length prefixes, checked *before* any
// allocation. Without these, a peer can send a length like `*777777772\r\n`
// and make the server try to allocate hundreds of millions of slice slots
// -- an easy memory-exhaustion DoS against an otherwise-correct parser.
// Values follow Redis's own defaults (proto-max-bulk-len=512MB); the array
// bound is far more generous than any command this cache actually needs.
const (
	maxBulkLength  = 512 * 1024 * 1024 // 512 MiB, matches Redis's default proto-max-bulk-len
	maxArrayLength = 1024 * 1024       // 1M elements
)

// ParseRESP reads exactly one RESP value from reader. Because reader is a
// *bufio.Reader over a blocking socket, a short read simply blocks until
// more bytes arrive -- this is what makes partial TCP packets, pipelined
// commands, and persistent connections all "just work" without any extra
// buffering logic here: the caller can call ParseRESP again immediately to
// pick up the next pipelined command already sitting in the buffer.
func ParseRESP(reader *bufio.Reader) (RespValue, error) {
	line, err := readLine(reader)
	if err != nil {
		return RespValue{}, err
	}
	if len(line) == 0 {
		return RespValue{}, fmt.Errorf("resp: empty line")
	}

	prefix := line[0]
	rest := line[1:]

	switch prefix {
	case '+':
		return SimpleString(rest), nil
	case '-':
		return Error(rest), nil
	case ':':
		n, err := strconv.ParseInt(rest, 10, 64)
		if err != nil {
			return RespValue{}, fmt.Errorf("resp: invalid integer %q: %w", rest, err)
		}
		return Integer(n), nil
	case '$':
		n, err := strconv.Atoi(rest)
		if err != nil {
			return RespValue{}, fmt.Errorf("resp: invalid bulk length %q: %w", rest, err)
		}
		if n < -1 {
			return RespValue{}, fmt.Errorf("resp: negative bulk length %d", n)
		}
		if n == -1 {
			return Null(), nil
		}
		if n > maxBulkLength {
			return RespValue{}, fmt.Errorf("resp: bulk length %d exceeds max of %d", n, maxBulkLength)
		}
		buf := make([]byte, n+2) // payload + trailing \r\n
		if _, err := io.ReadFull(reader, buf); err != nil {
			return RespValue{}, err
		}
		return Bulk(buf[:n]), nil
	case '*':
		n, err := strconv.Atoi(rest)
		if err != nil {
			return RespValue{}, fmt.Errorf("resp: invalid array length %q: %w", rest, err)
		}
		if n < -1 {
			return RespValue{}, fmt.Errorf("resp: negative array length %d", n)
		}
		if n == -1 {
			return Null(), nil
		}
		if n > maxArrayLength {
			return RespValue{}, fmt.Errorf("resp: array length %d exceeds max of %d", n, maxArrayLength)
		}
		items := make([]RespValue, n)
		for i := 0; i < n; i++ {
			item, err := ParseRESP(reader)
			if err != nil {
				return RespValue{}, err
			}
			items[i] = item
		}
		return Array(items), nil
	default:
		return RespValue{}, fmt.Errorf("resp: unknown type byte %q", line[0])
	}
}

// ParseCommand reads one client request and returns it as a plain string
// slice -- client requests are always a RESP Array of Bulk Strings, e.g.
// `*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n`.
func ParseCommand(reader *bufio.Reader) ([]string, error) {
	v, err := ParseRESP(reader)
	if err != nil {
		return nil, err
	}
	if v.Type != RespArray {
		return nil, fmt.Errorf("resp: expected a command array, got type %d", v.Type)
	}
	args := make([]string, len(v.Array))
	for i, item := range v.Array {
		if item.Type != RespBulkString {
			return nil, fmt.Errorf("resp: expected bulk string command arguments")
		}
		args[i] = string(item.Bulk)
	}
	return args, nil
}

func readLine(r *bufio.Reader) (string, error) {
	line, err := r.ReadString('\n')
	if err != nil {
		return "", err
	}
	return strings.TrimRight(line, "\r\n"), nil
}
