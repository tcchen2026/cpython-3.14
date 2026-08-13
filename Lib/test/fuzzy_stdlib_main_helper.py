try:
    value = fuzzy_missing_stdlib_main_name
except NameError:
    print("stdlib-main-native")
else:
    print("stdlib-main-fuzzy", type(value).__name__)
