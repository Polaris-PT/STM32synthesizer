# 音色文本格式（`.syn`）

纯文本、`key = value`、每行一个字段，支持空行与注释（`#`、`;`、`//`）。
未出现的字段保持原值；解析失败返回负数错误码（见下）。
放在 SD 卡的 `/synth/NNN.syn`（NNN = Program Change 音色号，3 位十进制）即可被自动加载。

## 示例
```ini
# Warm Pad
name             = Warm Pad
osc1.model       = saw          # 也可写数字：0=saw 1=square 2=pulse 3=tri 4=sine 5=supersaw 6=noise
osc2.model       = saw
osc2.detune      = -6           # 音分，-128..127
osc2.level       = 150
sub.level        = 40
filter.type      = lp24         # lp12/lp24/bp12/hp12
filter.cutoff    = 96           # 0..255 → 20Hz..18kHz 指数
filter.res       = 30
filter.keytrack  = 160          # 0..255 → 0..1
filter.env       = 40           # -128..127（滤波包络深度）
amp.a            = 70           # 0..255 → 0.5ms..8s（指数）
amp.d            = 120
amp.s            = 200          # 0..255 → 0..1
amp.r            = 140
flt.a            = 60
lfo.shape        = sine         # sine/tri/square/sh
lfo.rate         = 45
lfo.depth        = 25
lfo.dest         = filter       # pitch/filter/amp/pw
fx.chorus        = 90
fx.delay         = 0
fx.delay_fb      = 0
fx.drive         = 20
volume           = 200
```

## 全部字段
| key | 类型 | 范围/取值 |
|---|---|---|
| name | 字符串 | ≤15 字符 |
| osc1.model / osc2.model | 枚举或数字 | saw/square/pulse/tri/sine/supersaw/noise |
| osc1.pw / osc2.pw | 0..255 | 0.05..0.95 占空比 |
| osc2.semi | -128..127 | 半音 |
| osc2.detune | -128..127 | 音分 |
| osc1.level / osc2.level / sub.level / noise.level | 0..255 | |
| filter.type | 枚举或数字 | lp12/lp24/bp12/hp12 |
| filter.cutoff / filter.res / filter.keytrack | 0..255 | |
| filter.env | -128..127 | |
| amp.a / amp.d / amp.s / amp.r | 0..255 | 起音/衰减/保持/释音 |
| flt.a / flt.d / flt.s / flt.r | 0..255 | 滤波包络 |
| lfo.shape | 枚举或数字 | sine/tri/square/sh |
| lfo.rate / lfo.depth | 0..255 | 0.05..20Hz / 深度 |
| lfo.dest | 枚举或数字 | pitch/filter/amp/pw |
| glide | 0..255 | 0.5ms..1.5s |
| vel.amp / vel.cut | 0..255 | 力度灵敏度 |
| fx.chorus / fx.delay / fx.delay_fb / fx.drive | 0..255 | |
| volume | 0..255 | |

## 错误码（`patch_from_text` 返回值）
| 值 | 含义 |
|---|---|
| ≥0 | 成功应用的字段数 |
| -1 | 参数为空 |
| -2 | 行内缺少 `=` |
| -3 | 键为空 |
| -4 | 未知键 |
| -5 | 枚举越界/名字不识别 |
| -6 | 期望数字 |
| -7 | 数值越界 |
