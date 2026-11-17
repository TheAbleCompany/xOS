-- Comprehensive lua-1.0 test for ArtyomX64 mini-libc
print("=== arithmetic ===")
print(2 + 3)
print(10 - 4.5)
print(6 * 7)
print(9 / 4)
print(1024)

print("=== strings ===")
s = "Hello, " .. "World!"
print(s)
print(strlen(s))
print(strsub(s, 1, 5))
print(strupper("abc") .. strlower("DEF"))

print("=== math ===")
print(sin(0))
print(cos(0))
print(floor(3.7))
print(ceil(3.2))
print(sqrt(144))
print(abs(0 - 5))
print(min(10, 3, 7))
print(max(10, 3, 7))
print(mod(10, 3))

print("=== variables and control ===")
x = 0
i = 0
while i < 5 do
  i = i + 1
  x = x + i
end
print(x)

if x = 15 then
  print("fifteen!")
else
  print("not fifteen")
end

print("=== tables ===")
t = @()
t[1] = "one"
t[2] = "two"
t["name"] = "artyom"
print(t[1])
print(t[2])
print(t["name"])
print(t.name)

print("=== functions ===")
function add(a, b)
  return a + b
end
print(add(20, 22))

print("=== float formatting ===")
print(1/3)
print(2/3)
print(1000000)
print(0.5)

print("=== done ===")
