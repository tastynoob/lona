# Generic Container Sketch

这个文件是一个面向未来的 API 草图，不是当前编译器可直接运行的样例。

它想表达的是：

- 如果 generic v0 的实例化和 trait bound 都补齐
- `Vec` / `List` / `Map` 这类容器应该怎样与 `trait` 协作
- 迭代器如何作为独立 trait 暴露统一接口

当前未落地的关键前提包括：

- generic runtime instantiation / monomorphization
- generic trait / trait bound
- generic container 的 concrete layout materialization

参考实现边界：

- [docs/internals/compiler/generic_v0.md](../docs/internals/compiler/generic_v0.md)
- [docs/reference/language/trait.md](../docs/reference/language/trait.md)

## 目标形态

```lona
trait Iter[T] {
    def done() bool
    set def next() T
}

trait Sequence[T] {
    def len() i32
    def get(index i32) T
}

trait Assoc[K, V] {
    def len() i32
    def contains(key K) bool
    def get_or(key K, fallback V) V
}

struct Vec[T] {
    set data T[*]
    set len i32
    set cap i32

    set def push(value T)
    def get(index i32) T
    def iter() VecIter![T]
}

struct List[T] {
    set head ListNode![T]*
    set len i32

    set def append(value T)
    def iter() ListIter![T]
}

struct Map[K, V] {
    set keys K[*]
    set values V[*]
    set len i32

    set def put(key K, value V)
    def get_or(key K, fallback V) V
    def iter() MapIter![K, V]
}

impl[T] Vec![T]: Sequence![T]
impl[T] List![T]: Sequence![T]
impl[K, V] Map![K, V]: Assoc![K, V]

impl[T] VecIter![T]: Iter![T]
impl[T] ListIter![T]: Iter![T]
impl[K, V] MapIter![K, V]: Iter![Entry![K, V]]
```

## 希望支持的容器帮助函数

```lona
def collect_sum(iter Iter![i32] dyn) i32 {
    var out i32 = 0
    for iter.done() == false {
        out = out + iter.next()
    }
    ret out
}

def first_or_default[T](seq Sequence![T] const dyn, fallback T) T {
    if seq.len() == 0 {
        ret fallback
    }
    ret seq.get(0)
}

def map_lookup_or_zero(map Assoc![i32, i32] const dyn, key i32) i32 {
    ret map.get_or(key, 0)
}
```

## 期望的用法

```lona
var vec = Vec[i32]()
vec.push(1)
vec.push(2)
vec.push(3)

var list = List[i32]()
list.append(4)
list.append(5)

var map = Map[i32, i32]()
map.put(1, 10)
map.put(2, 20)

var seq Sequence![i32] const dyn = cast[Sequence![i32] dyn](&vec)
var assoc Assoc![i32, i32] const dyn = cast[Assoc![i32, i32] dyn](&map)

var vec_iter = vec.iter()
var iter Iter![i32] dyn = cast[Iter![i32] dyn](&vec_iter)

var total = collect_sum(iter)
var first = first_or_default(seq, 0)
var found = map_lookup_or_zero(assoc, 2)
```

## 设计目标

- 容器本身通过 trait 暴露最小公共接口，而不是把所有操作都塞进一个巨大的 trait。
- 迭代器保持独立的状态对象，统一用 `done()` + `next()` 协议。
- `Trait dyn` 继续作为显式动态路径；generic static path 仍应优先 direct call。
- 对只读容器使用 `Trait const dyn`，保证 getter-only 视图不会被 setter 动态调用放宽。
