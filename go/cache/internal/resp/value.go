// Package resp implements the RESP (REdis Serialization Protocol) wire
// format: parsing bytes off a buffered connection into commands, and
// encoding values back into replies.
package resp

// RespType identifies which of the RESP wire types a Value holds.
type RespType int

const (
	RespSimpleString RespType = iota
	RespError
	RespInteger
	RespBulkString
	RespArray
	RespNull
)

// RespValue is a single RESP value. Only the field matching Type is
// meaningful.
type RespValue struct {
	Type    RespType
	String  string      // RespSimpleString / RespError
	Integer int64       // RespInteger
	Bulk    []byte      // RespBulkString (binary-safe)
	Array   []RespValue // RespArray
}

func SimpleString(s string) RespValue { return RespValue{Type: RespSimpleString, String: s} }
func Error(msg string) RespValue      { return RespValue{Type: RespError, String: msg} }
func Integer(n int64) RespValue       { return RespValue{Type: RespInteger, Integer: n} }
func Bulk(b []byte) RespValue         { return RespValue{Type: RespBulkString, Bulk: b} }
func BulkStr(s string) RespValue      { return RespValue{Type: RespBulkString, Bulk: []byte(s)} }
func Array(items []RespValue) RespValue { return RespValue{Type: RespArray, Array: items} }
func Null() RespValue                 { return RespValue{Type: RespNull} }

// IsNull reports whether v represents a nil reply (a missing key, etc).
func (v RespValue) IsNull() bool { return v.Type == RespNull }
