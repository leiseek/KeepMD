# Mermaid flowchart compatibility

Native rendering examples.

## TD / rectangle

```mermaid
flowchart TD
A[开始] -->|标签| B[完成]
```

## TD / round

```mermaid
flowchart TD
A(开始) -->|标签| B[完成]
```

## TD / decision

```mermaid
flowchart TD
A{判断} -->|标签| B[完成]
```

## TD / circle

```mermaid
flowchart TD
A((圆形)) -->|标签| B[完成]
```

## TD / stadium

```mermaid
flowchart TD
A([结束]) -->|标签| B[完成]
```

## TD / quoted

```mermaid
flowchart TD
A["中文 label"] -->|标签| B[完成]
```

## TB / rectangle

```mermaid
flowchart TB
A[开始] -->|标签| B[完成]
```

## TB / round

```mermaid
flowchart TB
A(开始) -->|标签| B[完成]
```

## TB / decision

```mermaid
flowchart TB
A{判断} -->|标签| B[完成]
```

## TB / circle

```mermaid
flowchart TB
A((圆形)) -->|标签| B[完成]
```

## TB / stadium

```mermaid
flowchart TB
A([结束]) -->|标签| B[完成]
```

## TB / quoted

```mermaid
flowchart TB
A["中文 label"] -->|标签| B[完成]
```

## BT / rectangle

```mermaid
flowchart BT
A[开始] -->|标签| B[完成]
```

## BT / round

```mermaid
flowchart BT
A(开始) -->|标签| B[完成]
```

## BT / decision

```mermaid
flowchart BT
A{判断} -->|标签| B[完成]
```

## BT / circle

```mermaid
flowchart BT
A((圆形)) -->|标签| B[完成]
```

## BT / stadium

```mermaid
flowchart BT
A([结束]) -->|标签| B[完成]
```

## BT / quoted

```mermaid
flowchart BT
A["中文 label"] -->|标签| B[完成]
```

## LR / rectangle

```mermaid
flowchart LR
A[开始] -->|标签| B[完成]
```

## LR / round

```mermaid
flowchart LR
A(开始) -->|标签| B[完成]
```

## LR / decision

```mermaid
flowchart LR
A{判断} -->|标签| B[完成]
```

## LR / circle

```mermaid
flowchart LR
A((圆形)) -->|标签| B[完成]
```

## LR / stadium

```mermaid
flowchart LR
A([结束]) -->|标签| B[完成]
```

## LR / quoted

```mermaid
flowchart LR
A["中文 label"] -->|标签| B[完成]
```

## RL / rectangle

```mermaid
flowchart RL
A[开始] -->|标签| B[完成]
```

## RL / round

```mermaid
flowchart RL
A(开始) -->|标签| B[完成]
```

## RL / decision

```mermaid
flowchart RL
A{判断} -->|标签| B[完成]
```

## RL / circle

```mermaid
flowchart RL
A((圆形)) -->|标签| B[完成]
```

## RL / stadium

```mermaid
flowchart RL
A([结束]) -->|标签| B[完成]
```

## RL / quoted

```mermaid
flowchart RL
A["中文 label"] -->|标签| B[完成]
```

## Cycle

```mermaid
graph TD
A[开始] --> B{判断}
B -->|继续| A
B -->|结束| C[结束]
```

## Unsupported syntax

```mermaid
flowchart TD
A@{ shape: rounded } --> B
```
