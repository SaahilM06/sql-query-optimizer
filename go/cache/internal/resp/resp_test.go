package resp

import (
	"bufio"
	"bytes"
	"testing"
)

func roundTrip(t *testing.T, v RespValue) RespValue {
	t.Helper()
	var buf bytes.Buffer
	w := bufio.NewWriter(&buf)
	if err := WriteRESP(w, v); err != nil {
		t.Fatalf("WriteRESP: %v", err)
	}
	if err := w.Flush(); err != nil {
		t.Fatalf("Flush: %v", err)
	}
	r := bufio.NewReader(&buf)
	got, err := ParseRESP(r)
	if err != nil {
		t.Fatalf("ParseRESP on %q: %v", buf.String(), err)
	}
	return got
}

func TestSimpleStringRoundTrip(t *testing.T) {
	got := roundTrip(t, SimpleString("OK"))
	if got.Type != RespSimpleString || got.String != "OK" {
		t.Fatalf("got %+v", got)
	}
}

func TestErrorRoundTrip(t *testing.T) {
	got := roundTrip(t, Error("ERR bad command"))
	if got.Type != RespError || got.String != "ERR bad command" {
		t.Fatalf("got %+v", got)
	}
}

func TestIntegerRoundTrip(t *testing.T) {
	got := roundTrip(t, Integer(-42))
	if got.Type != RespInteger || got.Integer != -42 {
		t.Fatalf("got %+v", got)
	}
}

func TestBulkStringRoundTrip(t *testing.T) {
	got := roundTrip(t, Bulk([]byte("hello world")))
	if got.Type != RespBulkString || string(got.Bulk) != "hello world" {
		t.Fatalf("got %+v", got)
	}
}

func TestBulkStringBinarySafe(t *testing.T) {
	payload := []byte{0x00, 0x01, '\r', '\n', 0xff, 'x'}
	got := roundTrip(t, Bulk(payload))
	if got.Type != RespBulkString || !bytes.Equal(got.Bulk, payload) {
		t.Fatalf("binary payload mangled: got %v want %v", got.Bulk, payload)
	}
}

func TestNullRoundTrip(t *testing.T) {
	got := roundTrip(t, Null())
	if !got.IsNull() {
		t.Fatalf("expected null, got %+v", got)
	}
}

func TestArrayRoundTrip(t *testing.T) {
	v := Array([]RespValue{BulkStr("SET"), BulkStr("key"), BulkStr("value")})
	got := roundTrip(t, v)
	if got.Type != RespArray || len(got.Array) != 3 {
		t.Fatalf("got %+v", got)
	}
	if string(got.Array[0].Bulk) != "SET" || string(got.Array[2].Bulk) != "value" {
		t.Fatalf("element mismatch: %+v", got.Array)
	}
}

func TestParseCommand(t *testing.T) {
	raw := "*2\r\n$3\r\nGET\r\n$6\r\nplan:1\r\n"
	r := bufio.NewReader(bytes.NewReader([]byte(raw)))
	args, err := ParseCommand(r)
	if err != nil {
		t.Fatalf("ParseCommand: %v", err)
	}
	if len(args) != 2 || args[0] != "GET" || args[1] != "plan:1" {
		t.Fatalf("got %v", args)
	}
}

func TestParseCommandRejectsNonArray(t *testing.T) {
	r := bufio.NewReader(bytes.NewReader([]byte("+OK\r\n")))
	if _, err := ParseCommand(r); err == nil {
		t.Fatal("expected error for a non-array command frame")
	}
}

// Two commands pipelined back to back in a single buffer -- ParseCommand
// must be callable repeatedly against the same reader to drain both without
// any extra framing help from the caller.
func TestPipelinedCommands(t *testing.T) {
	raw := "*1\r\n$4\r\nPING\r\n*3\r\n$3\r\nSET\r\n$1\r\na\r\n$1\r\n1\r\n"
	r := bufio.NewReader(bytes.NewReader([]byte(raw)))

	first, err := ParseCommand(r)
	if err != nil || len(first) != 1 || first[0] != "PING" {
		t.Fatalf("first command: %v %v", first, err)
	}
	second, err := ParseCommand(r)
	if err != nil || len(second) != 3 || second[0] != "SET" {
		t.Fatalf("second command: %v %v", second, err)
	}
}

// A command split across what would be multiple TCP reads: bufio.Reader
// backed by an io.Reader that returns data in small chunks still parses
// correctly because ParseRESP blocks on ReadFull/ReadString until enough
// bytes exist, rather than assuming one read equals one frame.
type slowReader struct {
	data []byte
	pos  int
}

func (s *slowReader) Read(p []byte) (int, error) {
	if s.pos >= len(s.data) {
		return 0, bytesEOF
	}
	n := 1 // one byte at a time -- worst-case fragmentation
	if s.pos+n > len(s.data) {
		n = len(s.data) - s.pos
	}
	copy(p, s.data[s.pos:s.pos+n])
	s.pos += n
	return n, nil
}

var bytesEOF = errEOF{}

type errEOF struct{}

func (errEOF) Error() string { return "EOF" }

func TestFragmentedReadStillParses(t *testing.T) {
	raw := []byte("*2\r\n$3\r\nGET\r\n$6\r\nplan:1\r\n")
	r := bufio.NewReader(&slowReader{data: raw})
	args, err := ParseCommand(r)
	if err != nil {
		t.Fatalf("ParseCommand over fragmented reads: %v", err)
	}
	if len(args) != 2 || args[0] != "GET" || args[1] != "plan:1" {
		t.Fatalf("got %v", args)
	}
}

func TestMalformedBulkLength(t *testing.T) {
	r := bufio.NewReader(bytes.NewReader([]byte("$notanumber\r\n")))
	if _, err := ParseRESP(r); err == nil {
		t.Fatal("expected error for malformed bulk length")
	}
}

func TestMalformedTypeByte(t *testing.T) {
	r := bufio.NewReader(bytes.NewReader([]byte("?garbage\r\n")))
	if _, err := ParseRESP(r); err == nil {
		t.Fatal("expected error for unknown type byte")
	}
}

func TestNegativeBulkLengthRejected(t *testing.T) {
	r := bufio.NewReader(bytes.NewReader([]byte("$-5\r\n")))
	if _, err := ParseRESP(r); err == nil {
		t.Fatal("expected error for invalid negative bulk length")
	}
}

// Regression test for a fuzzer-found DoS: an attacker-controlled length
// prefix must be rejected before the parser allocates anything, not after.
func TestOversizedArrayLengthRejectedWithoutAllocating(t *testing.T) {
	r := bufio.NewReader(bytes.NewReader([]byte("*777777772\r\n")))
	_, err := ParseRESP(r)
	if err == nil {
		t.Fatal("expected error for an array length far beyond any real command")
	}
}

func TestOversizedBulkLengthRejectedWithoutAllocating(t *testing.T) {
	r := bufio.NewReader(bytes.NewReader([]byte("$99999999999\r\n")))
	_, err := ParseRESP(r)
	if err == nil {
		t.Fatal("expected error for a bulk length far beyond maxBulkLength")
	}
}

// FuzzParseRESP feeds arbitrary bytes to the parser -- it must never panic,
// only return an error, no matter how malformed the input is. Run with:
//
//	go test -fuzz=FuzzParseRESP ./internal/resp
func FuzzParseRESP(f *testing.F) {
	f.Add([]byte("+OK\r\n"))
	f.Add([]byte("-ERR oops\r\n"))
	f.Add([]byte(":123\r\n"))
	f.Add([]byte("$5\r\nhello\r\n"))
	f.Add([]byte("$-1\r\n"))
	f.Add([]byte("*2\r\n$3\r\nGET\r\n$1\r\na\r\n"))
	f.Add([]byte("*-1\r\n"))
	f.Add([]byte(""))
	f.Add([]byte("garbage"))

	f.Fuzz(func(t *testing.T, data []byte) {
		r := bufio.NewReader(bytes.NewReader(data))
		defer func() {
			if rec := recover(); rec != nil {
				t.Fatalf("ParseRESP panicked on input %q: %v", data, rec)
			}
		}()
		_, _ = ParseRESP(r)
	})
}
