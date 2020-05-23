# Install

```shell
sudo apt-get install -y clang wiringpi
make build/server/server build/inverter
``` 

### Test
```shell
sudo build/inverter /dev/hidraw0 QPIGS
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

Restart Postgres
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

# Install systemd service
```shell
sudo cp systemd/power.service /etc/systemd/system
# Edit /etc/systemd/system/power.service if your path is not /home/pi/homepower
sudo systemctl enable power
sudo systemctl start power
# Inspect output
sudo journalctl -u power
# Run this if you change parameters of your systemd service
sudo systemctl daemon-reload
```

# Docs, history
* https://github.com/ned-kelly/docker-voltronic-homeassistant
* http://forums.aeva.asn.au/uploads/293/HS_MS_MSX_RS232_Protocol_20140822_after_current_upgrade.pdf