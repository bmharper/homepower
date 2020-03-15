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
* Postgres credentials: username = pi, password = treefeller
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

