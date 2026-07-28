package storage

import "container/list"

// lru tracks key recency for one shard, separately from the entries
// themselves: the front of the list is most-recently-used, the back is
// least-recently-used (the next eviction candidate). Not safe for
// concurrent use on its own -- callers hold the owning shard's mutex.
type lru struct {
	list  *list.List
	items map[string]*list.Element
}

func newLRU() *lru {
	return &lru{
		list:  list.New(),
		items: make(map[string]*list.Element),
	}
}

// touch marks key as most-recently-used, inserting it at the front if it's
// not already tracked.
func (l *lru) touch(key string) {
	if el, ok := l.items[key]; ok {
		l.list.MoveToFront(el)
		return
	}
	el := l.list.PushFront(key)
	l.items[key] = el
}

// remove untracks key. No-op if key isn't tracked.
func (l *lru) remove(key string) {
	if el, ok := l.items[key]; ok {
		l.list.Remove(el)
		delete(l.items, key)
	}
}

// evictCandidate returns the current least-recently-used key without
// removing it, or "" if the LRU is empty.
func (l *lru) evictCandidate() (string, bool) {
	back := l.list.Back()
	if back == nil {
		return "", false
	}
	return back.Value.(string), true
}

func (l *lru) len() int { return l.list.Len() }
