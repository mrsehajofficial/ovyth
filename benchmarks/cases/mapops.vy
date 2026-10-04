m = {}
i = 0
while i < 100000 {
    m["key" + str(i)] = i
    i = i + 1
}
j = 0
s = 0
while j < 100000 {
    s = s + m["key" + str(j)]
    j = j + 1
}
print(len(m), s)
