import datetime

print("Content-Type: text/html")
print()
print(f"""
<html>
<body>
  <h1>Hello from Python</h1>
  <p>The time is: {datetime.datetime.now()}</p>
  <p>This output was captured from stdout by s1mple.</p>
</body>
</html>
""")