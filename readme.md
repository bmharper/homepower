# Install

* sudo apt-get install -y clang wiringpi

# Postgres setup
### Install Postgres
* sudo apt install postgresql
### Create 'pi' user
```shell
sudo su postgres
createuser pi -P --interactive
```
* Postgres credentials: username = pi, password = homepower
### Create 'power' database
```shell
psql postgres
create database power;
```
### Setup database schema
```shell
psql power -U pi -f dbcreate.sql
```
### Connect to database
```shell
psql power -U pi
```

# Docs, history
* https://github.com/ned-kelly/docker-voltronic-homeassistant
* http://forums.aeva.asn.au/uploads/293/HS_MS_MSX_RS232_Protocol_20140822_after_current_upgrade.pdf