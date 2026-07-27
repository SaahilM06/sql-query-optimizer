package main

import (
	"reflect"
	"testing"
)

func TestTokenizeSimple(t *testing.T) {
	got, err := tokenize("SET key value")
	if err != nil {
		t.Fatalf("tokenize: %v", err)
	}
	want := []string{"SET", "key", "value"}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("got %v, want %v", got, want)
	}
}

func TestTokenizeDoubleQuotedValueWithSpaces(t *testing.T) {
	got, err := tokenize(`SET key "hello world"`)
	if err != nil {
		t.Fatalf("tokenize: %v", err)
	}
	want := []string{"SET", "key", "hello world"}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("got %v, want %v", got, want)
	}
}

func TestTokenizeSingleQuotedValue(t *testing.T) {
	got, err := tokenize(`SET key 'hello world'`)
	if err != nil {
		t.Fatalf("tokenize: %v", err)
	}
	want := []string{"SET", "key", "hello world"}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("got %v, want %v", got, want)
	}
}

func TestTokenizeUnterminatedQuoteIsError(t *testing.T) {
	if _, err := tokenize(`SET key "unterminated`); err == nil {
		t.Fatal("expected an error for an unterminated quote")
	}
}

func TestTokenizeExtraWhitespaceCollapses(t *testing.T) {
	got, err := tokenize("  GET    key  ")
	if err != nil {
		t.Fatalf("tokenize: %v", err)
	}
	want := []string{"GET", "key"}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("got %v, want %v", got, want)
	}
}
