const now = new Date().toISOString();
process.stdout.write(`
<html>
<body>
  <h1>Hello from Node.js</h1>
  <p>The time is: ${now}</p>
  <p>This output was captured from stdout by s1mple.</p>
</body>
</html>
`);