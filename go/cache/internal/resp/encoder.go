package resp

import (
	"bufio"
	"strconv"
)

// WriteRESP encodes value to writer. It does not flush -- callers decide
// when to flush (e.g. once per response, or once per batch of pipelined
// responses) so the connection layer controls the write/flush granularity.
func WriteRESP(writer *bufio.Writer, value RespValue) error {
	switch value.Type {
	case RespSimpleString:
		if _, err := writer.WriteString("+" + value.String + "\r\n"); err != nil {
			return err
		}
	case RespError:
		if _, err := writer.WriteString("-" + value.String + "\r\n"); err != nil {
			return err
		}
	case RespInteger:
		if _, err := writer.WriteString(":" + strconv.FormatInt(value.Integer, 10) + "\r\n"); err != nil {
			return err
		}
	case RespBulkString:
		if _, err := writer.WriteString("$" + strconv.Itoa(len(value.Bulk)) + "\r\n"); err != nil {
			return err
		}
		if _, err := writer.Write(value.Bulk); err != nil {
			return err
		}
		if _, err := writer.WriteString("\r\n"); err != nil {
			return err
		}
	case RespArray:
		if _, err := writer.WriteString("*" + strconv.Itoa(len(value.Array)) + "\r\n"); err != nil {
			return err
		}
		for _, item := range value.Array {
			if err := WriteRESP(writer, item); err != nil {
				return err
			}
		}
	case RespNull:
		// Represented on the wire as a null bulk string -- the form every
		// command in this cache actually needs (a missing GET, etc).
		if _, err := writer.WriteString("$-1\r\n"); err != nil {
			return err
		}
	}
	return nil
}

// EncodeCommand encodes args as a RESP array of bulk strings -- the format
// every client request (and this package's own CLI/benchmark clients) uses.
func EncodeCommand(args ...string) RespValue {
	items := make([]RespValue, len(args))
	for i, a := range args {
		items[i] = BulkStr(a)
	}
	return Array(items)
}
