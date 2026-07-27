package main

import "testing"

func TestCommaInt(t *testing.T) {
	cases := map[int]string{
		0:         "0",
		42:        "42",
		999:       "999",
		1000:      "1,000",
		100000:    "100,000",
		1234567:   "1,234,567",
	}
	for in, want := range cases {
		if got := commaInt(in); got != want {
			t.Errorf("commaInt(%d) = %q, want %q", in, got, want)
		}
	}
}

func TestCommaFloatWholeNumber(t *testing.T) {
	if got := commaFloat(74200); got != "74,200" {
		t.Errorf("got %q", got)
	}
}

func TestCommaFloatWithFraction(t *testing.T) {
	got := commaFloat(74212.7)
	if got != "74,212.70" {
		t.Errorf("got %q, want %q", got, "74,212.70")
	}
}

func TestPercentileEmptySliceReturnsZero(t *testing.T) {
	if got := percentile(nil, 0.5); got != 0 {
		t.Errorf("got %v, want 0", got)
	}
}
