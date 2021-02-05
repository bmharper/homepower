# Install

## Install git and build tools
```shell
sudo apt install git build-essential clang wiringpi
```

## Download and build
```shell
git clone https://github.com/bmharper/homepower
cd homepower
git submodule update --init --recursive
make -j build/server/server build/inverter
```

## Test
```shell
sudo build/inverter /dev/hidraw0 QPIGS
```
If the test is successful, then you should see something like this:
```json
{
    "ACInHz": 50.0,
    "ACInV": 248.4,
    "ACOutHz": 50.0,
    "ACOutV": 230.6,
    "BatChA": 0.0,
    "BatP": 85.0,
    "BatV": 27.0,
    "BusV": 427.0,
    "LoadP": 13.0,
    "LoadVA": 414.0,
    "LoadW": 339.0,
    "PvA": 0.9,
    "PvV": 254.7,
    "PvW": 229.0,
    "Raw": "(248.4 50.0 230.6 50.0 0414 0339 013 427 27.00 000 085 0030 00.9 254.7 00.00 00003 10010000 00 00 00229 010",
    "Temp": 30.0,
    "Unknown1": 0.0,
    "Unknown2": "00003",
    "Unknown3": "10010000",
    "Unknown4": "00",
    "Unknown5": "00",
    "Unknown6": "010"
}
```

# Postgres setup
### Install Postgres
```shell
sudo apt install postgresql
```

### Enabling Remote Postgres Access
If you want to run grafana (or anything else) from another machine,
and connect it to Postgres on the RaspberryPi, then you must enable
remote access to the Postgres DB, eg.

In `/etc/postgresql/11/main/pg_hba.conf`:
```
host    all             all             0.0.0.0/0            md5
```

In `/etc/postgresql/11/main/postgresql.conf`:
```
listen_addresses = '*'
```

### Restart Postgres
```shell
sudo systemctl restart postgresql
# Make sure everything is still OK
sudo journalctl -u postgresql
```

### Create `pi` user with password `homepower`
```shell
sudo su postgres
createuser pi -P --interactive
# Enter password: homepower
# Superuser: yes
\q
```
### Create `power` database
```shell
psql -h localhost -U pi postgres
create database power;
\q
```
### Setup database schema
```shell
psql -h localhost -U pi --dbname power -f dbcreate.sql
```
### Connect to database
```shell
psql -h localhost -U pi --dbname power
select * from readings;
\q
```

# Install power monitor as a systemd service
You'll want to do this so that the monitor starts up with your system
```shell
sudo cp systemd/power.service /etc/systemd/system
# Edit /etc/systemd/system/power.service if your path is not /home/pi/homepower
# Also, if you want the 'controller' mode switched on (you probably don't!), then
# change the command inside power.service to ExecStart=/home/pi/homepower/build/server/server -c
sudo systemctl enable power
sudo systemctl start power
# Inspect output
sudo journalctl -u power
# Run this if you change parameters of your systemd service
sudo systemctl daemon-reload
```

# Install grafana
If you want to see pretty graphs of your data, then I recommend installing Grafana.
Grafana runs just fine a modern Raspberry PI. Just search the internet for instructions on
[how to install it](https://grafana.com/tutorials/install-grafana-on-raspberry-pi).

In order to setup Grafana you'll need these details for connecting to the Postgres database:
|||
|-|-|
|Host    |localhost|
|Database|power|
|Username|pi|
|Password|homepower|

All the data comes from the `readings` table. The time column is `time`, and the rest should be self-explanatory.

# Docs, history
* https://github.com/ned-kelly/docker-voltronic-homeassistant
* http://forums.aeva.asn.au/uploads/293/HS_MS_MSX_RS232_Protocol_20140822_after_current_upgrade.pdf