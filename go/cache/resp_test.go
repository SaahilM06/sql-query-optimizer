package cache

import (
	"bufio"
	"bytes"
	"testing"
)

func roundTrip(t *testing.T, v Value) Value {
	t.Helper()
	encoded := v.Encode()
	r := bufio.NewReader(bytes.NewReader(encoded))
	got, err := ReadValue(r)
	if err != nil {
		t.Fatalf("ReadValue failed on %q: %v", encoded, err)
	}
	return got
}

func TestEncodeSimpleString(t *testing.T) {
	v := SimpleString("OK")
	if got := string(v.Encode()); got != "+OK\r\n" {
		t.Fatalf("got %q", got)
	}
	got := roundTrip(t, v)
	if got.Type != TypeSimpleString || got.Str != "OK" {
		t.Fatalf("round trip mismatch: %+v", got)
	}
}

func TestEncodeError(t *testing.T) {
	v := Error("ERR bad command")
	if got := string(v.Encode()); got != "-ERR bad command\r\n" {
		t.Fatalf("got %q", got)
	}
	got := roundTrip(t, v)
	if got.Type != TypeError || got.Str != "ERR bad command" {
		t.Fatalf("round trip mismatch: %+v", got)
	}
}

func TestEncodeInteger(t *testing.T) {
	v := Integer(-2)
	if got := string(v.Encode()); got != ":-2\r\n" {
		t.Fatalf("got %q", got)
	}
	got := roundTrip(t, v)
	if got.Type != TypeInteger || got.Int != -2 {
		t.Fatalf("round trip mismatch: %+v", got)
	}
}

func TestEncodeBulkString(t *testing.T) {
	v := BulkString([]byte("hello"))
	if got := string(v.Encode()); got != "$5\r\nhello\r\n" {
		t.Fatalf("got %q", got)
	}
	got := roundTrip(t, v)
	if got.Type != TypeBulkString || string(got.Bulk) != "hello" {
		t.Fatalf("round trip mismatch: %+v", got)
	}
}

func TestEncodeNullBulk(t *testing.T) {
	v := NullBulk()
	if got := string(v.Encode()); got != "$-1\r\n" {
		t.Fatalf("got %q", got)
	}
	got := roundTrip(t, v)
	if got.Type != TypeBulkString || !got.Null {
		t.Fatalf("round trip mismatch: %+v", got)
	}
}

func TestEncodeArrayOfBulkStrings(t *testing.T) {
	v := Array([]Value{BulkString([]byte("GET")), BulkString([]byte("plan:1"))})
	want := "*2\r\n$3\r\nGET\r\n$6\r\nplan:1\r\n"
	if got := string(v.Encode()); got != want {
		t.Fatalf("got %q, want %q", got, want)
	}
	got := roundTrip(t, v)
	if got.Type != TypeArray || len(got.Array) != 2 {
		t.Fatalf("round trip mismatch: %+v", got)
	}
	if string(got.Array[0].Bulk) != "GET" || string(got.Array[1].Bulk) != "plan:1" {
		t.Fatalf("round trip element mismatch: %+v", got.Array)
	}
}

func TestEncodeNullArray(t *testing.T) {
	v := NullArray()
	if got := string(v.Encode()); got != "*-1\r\n" {
		t.Fatalf("got %q", got)
	}
	got := roundTrip(t, v)
	if got.Type != TypeArray || !got.Null {
		t.Fatalf("round trip mismatch: %+v", got)
	}
}

func TestReadValueMalformed(t *testing.T) {
	r := bufio.NewReader(bytes.NewReader([]byte("$notanumber\r\n")))
	if _, err := ReadValue(r); err == nil {
		t.Fatal("expected an error for a malformed bulk-string length")
	}
}
