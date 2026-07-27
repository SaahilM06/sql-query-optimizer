package cache

import (
	"bufio"
	"bytes"
	"fmt"
	"io"
	"strconv"
	"strings"
)

// RespType is the leading byte of a RESP (REdis Serialization Protocol)
// frame that identifies which of the five wire types follows.
type RespType byte

const (
	TypeSimpleString RespType = '+'
	TypeError        RespType = '-'
	TypeInteger      RespType = ':'
	TypeBulkString   RespType = '$'
	TypeArray        RespType = '*'
)

// Value is a single RESP value. Only the fields relevant to Type are
// meaningful; e.g. a TypeInteger value only uses Int.
type Value struct {
	Type  RespType
	Str   string  // SimpleString / Error payload
	Int   int64   // Integer payload
	Bulk  []byte  // BulkString payload (ignored when Null)
	Array []Value // Array payload (ignored when Null)
	Null  bool    // true for a null bulk string ($-1) or null array (*-1)
}

func SimpleString(s string) Value { return Value{Type: TypeSimpleString, Str: s} }
func Error(msg string) Value      { return Value{Type: TypeError, Str: msg} }
func Integer(n int64) Value       { return Value{Type: TypeInteger, Int: n} }
func BulkString(b []byte) Value   { return Value{Type: TypeBulkString, Bulk: b} }
func NullBulk() Value             { return Value{Type: TypeBulkString, Null: true} }
func Array(items []Value) Value   { return Value{Type: TypeArray, Array: items} }
func NullArray() Value            { return Value{Type: TypeArray, Null: true} }

// Encode serializes v to its RESP wire representation.
func (v Value) Encode() []byte {
	switch v.Type {
	case TypeSimpleString:
		return []byte("+" + v.Str + "\r\n")
	case TypeError:
		return []byte("-" + v.Str + "\r\n")
	case TypeInteger:
		return []byte(":" + strconv.FormatInt(v.Int, 10) + "\r\n")
	case TypeBulkString:
		if v.Null {
			return []byte("$-1\r\n")
		}
		var buf bytes.Buffer
		buf.WriteByte('$')
		buf.WriteString(strconv.Itoa(len(v.Bulk)))
		buf.WriteString("\r\n")
		buf.Write(v.Bulk)
		buf.WriteString("\r\n")
		return buf.Bytes()
	case TypeArray:
		if v.Null {
			return []byte("*-1\r\n")
		}
		var buf bytes.Buffer
		buf.WriteByte('*')
		buf.WriteString(strconv.Itoa(len(v.Array)))
		buf.WriteString("\r\n")
		for _, item := range v.Array {
			buf.Write(item.Encode())
		}
		return buf.Bytes()
	default:
		return nil
	}
}

// ReadValue parses the next RESP value from r. Client requests are always
// a RESP Array of Bulk Strings (e.g. `*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n`);
// server responses may be any of the five types.
func ReadValue(r *bufio.Reader) (Value, error) {
	line, err := readLine(r)
	if err != nil {
		return Value{}, err
	}
	if len(line) == 0 {
		return Value{}, fmt.Errorf("resp: empty line")
	}

	prefix := RespType(line[0])
	rest := line[1:]

	switch prefix {
	case TypeSimpleString:
		return SimpleString(rest), nil
	case TypeError:
		return Error(rest), nil
	case TypeInteger:
		n, err := strconv.ParseInt(rest, 10, 64)
		if err != nil {
			return Value{}, fmt.Errorf("resp: invalid integer %q: %w", rest, err)
		}
		return Integer(n), nil
	case TypeBulkString:
		n, err := strconv.Atoi(rest)
		if err != nil {
			return Value{}, fmt.Errorf("resp: invalid bulk length %q: %w", rest, err)
		}
		if n == -1 {
			return NullBulk(), nil
		}
		buf := make([]byte, n+2) // payload + trailing \r\n
		if _, err := io.ReadFull(r, buf); err != nil {
			return Value{}, err
		}
		return BulkString(buf[:n]), nil
	case TypeArray:
		n, err := strconv.Atoi(rest)
		if err != nil {
			return Value{}, fmt.Errorf("resp: invalid array length %q: %w", rest, err)
		}
		if n == -1 {
			return NullArray(), nil
		}
		items := make([]Value, n)
		for i := 0; i < n; i++ {
			item, err := ReadValue(r)
			if err != nil {
				return Value{}, err
			}
			items[i] = item
		}
		return Array(items), nil
	default:
		return Value{}, fmt.Errorf("resp: unknown type byte %q", line[0])
	}
}

func readLine(r *bufio.Reader) (string, error) {
	line, err := r.ReadString('\n')
	if err != nil {
		return "", err
	}
	return strings.TrimRight(line, "\r\n"), nil
}
