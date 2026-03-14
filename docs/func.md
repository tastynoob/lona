# 函数声明示例

> 对应 `docs/grammer.md` 的“3.3 结构体与函数声明”。

## 1. 无参数、无显式返回类型

```lona
def noop() {}
```

## 2. 无参数，直接写返回类型

```lona
def one() i32 {
    ret 1
}
```

## 3. 有参数、无显式返回类型

```lona
def printNumber(v i32) {
    ret
}
```

## 4. 有参数，并直接写返回类型

```lona
def add(a i32, b i32) i32 {
    ret a + b
}
```

## 5. 参数类型可以是复杂类型

```lona
def load(ptr i32*, size i32) i32 {
    ret *ptr
}

def apply(f (i32) i32, x i32) i32 {
    ret f(x)
}
```

## 6. 函数体总是块语句

```lona
def abs(v i32) i32 {
    if v < 0 {
        ret -v
    }

    ret v
}
```
