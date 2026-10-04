try {
    print(1 / 0)
} catch e {
    print("caught:", e)
}
try {
    throw "boom"
} catch err {
    print("caught:", err)
}
print("survived")
