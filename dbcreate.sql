--DROP TABLE IF EXISTS readings;
CREATE TABLE readings (
	time TIMESTAMP NOT NULL PRIMARY KEY,
	acInV REAL,
	acInHz REAL,
	acOutV REAL,
	acOutHz REAL,
	loadVA REAL,
	loadW REAL,
	loadP REAL,
	busV REAL,
	batV REAL,
	batChA REAL,
	batP REAL,
	temp REAL,
	pvA REAL,
	pvV REAL,
	pvW REAL,
	heavy BOOLEAN
);