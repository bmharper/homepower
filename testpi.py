import time
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from gpiozero import LED

GRID_PIN = 17       # Pin "11" - aka GPIO 0 on Raspberry Pi Model A (1st gen)
INVERTER_PIN = 18   # Pin "12" - aka GPIO 1 on Raspberry Pi Model A (1st gen)

# At 50hz, a full cycle is 20ms
PAUSE_MILLISECONDS = 100

htmlSrc = '''
<!DOCTYPE HTML>
<head>
<style>
	html {
		font-family: sans-serif;
	}
	.status {
		display: flex;
		font-size: 60px;
		padding: 10px;
	}
	button {
		font-size: 80px;
		margin: 20px;
		padding: 10px 20px;
	}
</style>
</head>
<html>
<div class='status'>
	<div>Status: </div><div id='status'>STATUS</div>
</div>
<div>
	<button id='off'>Off</button>
	<button id='inverter'>Inverter</button>
	<button id='grid'>Grid</button>
	<br/>
	<button id='refresh'>Refresh</button>
</div>
<script>
function refresh() {
	fetch('/status').then((resp) => {
		return resp.text();
	}).then((txt) => {
		document.getElementById('status').innerText = txt;
	}).catch((err) => {
		alert(err);
	});
}
document.getElementById('off').addEventListener('click', () => {
	fetch('/switch/off', {method: 'POST'});
	refresh();
});
document.getElementById('inverter').addEventListener('click', () => {
	fetch('/switch/inverter', {method: 'POST'});
	refresh();
});
document.getElementById('grid').addEventListener('click', () => {
	fetch('/switch/grid', {method: 'POST'});
	refresh();
});
document.getElementById('refresh').addEventListener('click', () => {
	refresh();
});
</script>
</html>
'''

class MyServer():
	def __init__(self):
		self.lock = threading.Lock()
		self.relay_grid = LED(GRID_PIN, initial_value=False)
		self.relay_inverter = LED(INVERTER_PIN, initial_value=False)
		self.current_status = 'off'

	def switch_power(self, switch_to):
		with self.lock:
			if switch_to == self.current_status:
				return

			print(f"Switching to '{switch_to}'")
			if switch_to == 'inverter':
				self.relay_grid.off()
				time.sleep(PAUSE_MILLISECONDS / 1000)
				self.relay_inverter.on()
				self.current_status = switch_to
			elif switch_to == 'grid':
				self.relay_inverter.off()
				time.sleep(PAUSE_MILLISECONDS / 1000)
				self.relay_grid.on()
				self.current_status = switch_to
			elif switch_to == 'off':
				self.relay_inverter.off()
				self.relay_grid.off()
				self.current_status = switch_to
			else:
				print(f"Unknown mode {switch_to}")

myserver = MyServer()

class Server(BaseHTTPRequestHandler):
	def sendTextResponse(self, body):
		self.send_response(200)
		self.send_header('Content-type', 'text/plain')
		self.end_headers()
		self.wfile.write(body.encode('utf-8'))

	def sendHtmlResponse(self, body):
		self.send_response(200)
		self.send_header('Content-type', 'text/html')
		self.end_headers()
		self.wfile.write(body.encode('utf-8'))

	def do_GET(self):
		if self.path == '/':
			src = htmlSrc
			src = src.replace('STATUS', myserver.current_status)
			self.sendHtmlResponse(src)
		elif self.path == '/status':
			self.sendTextResponse(myserver.current_status)
		else:
			self.sendTextResponse("Unknown path")

	def do_POST(self):
		if self.path == '/switch/grid':
			myserver.switch_power('grid')
			self.sendTextResponse("OK")
		elif self.path == '/switch/inverter':
			myserver.switch_power('inverter')
			self.sendTextResponse("OK")
		elif self.path == '/switch/off':
			myserver.switch_power('off')
			self.sendTextResponse("OK")
		else:
			self.sendTextResponse("Unrecognized command")
		

port = 8080
print(f"Server starting on port {port}")
server = HTTPServer(('', port), Server)
server.serve_forever()

#switch_power(True)
#time.sleep(2)
#switch_power(False)
#time.sleep(2)
#switch_power(True)
#time.sleep(2)
