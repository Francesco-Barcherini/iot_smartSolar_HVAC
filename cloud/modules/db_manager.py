'''
class MySQLDatabase

method __init__ to connecto to database with config from app_config.py

method reset_db to reset the database by dropping all tables and creating them again

tables:
- Sensors (id, sensor, value, timestamp)
- Relay (id, solar_to, house_from, power_solar, power_home, timestamp) -> solar_to and house_from are integervalues coming from an enum
- AntiDust (timestamp) -> store the timestamp of each antidust operation
- HVAC (id, power, status, mode, target_temp, timestamp) -> power is a float, status and mode are integers coming from an enum

Create the method to insert data into the tables for each table
'''

import mysql.connector
from config import app_config

class MySQLDB():
    def __init__(self):
        print('Connecting to Database...')
        self._database = mysql.connector.connect(
            host=app_config.DB_HOST,
            user=app_config.DB_USER,
            password=app_config.DB_PASSWORD,
            database=app_config.DB_NAME
        )
        print("Connected to database ", self._database)

    def connect(self):
        if not self._database.is_connected():
            print("Reconnecting to MySQL...")
            self._database = mysql.connector.connect(
                host=app_config.DB_HOST,
                user=app_config.DB_USER,
                password=app_config.DB_PASSWORD,
                database=app_config.DB_NAME
            )
            print("Reconnected to database ", self._database)
        else:
            print("Already connected to the database")

    def reset_db(self):
        print('Resetting Database')
        reset_cursor = self._database.cursor()
        reset_cursor.execute("SHOW TABLES")
        tables = reset_cursor.fetchall()

        for table in tables:
            reset_cursor.execute(f'DROP TABLE {table[0]}')

        # Create tables
        reset_cursor.execute('''CREATE TABLE Sensors (
                                id INT AUTO_INCREMENT PRIMARY KEY,
                                sensor VARCHAR(50),
                                value FLOAT NOT NULL,
                                timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP)''')

        reset_cursor.execute('''CREATE TABLE Relay (
                                id INT AUTO_INCREMENT PRIMARY KEY,
                                solar_to INT,
                                house_from INT,
                                power_solar FLOAT NOT NULL,
                                power_home FLOAT NOT NULL,
                                timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP)''')

        reset_cursor.execute('''CREATE TABLE AntiDust (
                                id INT AUTO_INCREMENT PRIMARY KEY,
                                operation VARCHAR(50),
                                timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP)''')

        reset_cursor.execute('''CREATE TABLE HVAC (
                                id INT AUTO_INCREMENT PRIMARY KEY,
                                power FLOAT NOT NULL,
                                status INT NOT NULL,
                                mode INT NOT NULL,
                                target_temp FLOAT NOT NULL,
                                timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP)''')

        self._database.commit()
        print('Database reset completed')

    def ensure_mysql_connection(self, conn):
        try:
            conn.ping(reconnect=True)
        except Exception as e:
            print("🔁 Reconnecting to MySQL...")
            self.connect()


    def insert_sensor_data(self, sensor, value):
        cursor = self._database.cursor()
        cursor.execute(f'INSERT INTO Sensors (sensor, value) VALUES ("{sensor}", {value})')
        self._database.commit()
        if cursor.rowcount > 0:
            print(f'Sensor data inserted: {sensor} - {value}')

    def insert_relay_data(self, solar_to, house_from, power_solar, power_home):
        cursor = self._database.cursor()
        cursor.execute(f'INSERT INTO Relay (solar_to, house_from, power_solar, power_home) VALUES ({solar_to}, {house_from}, {power_solar}, {power_home})')
        self._database.commit()
        if cursor.rowcount > 0:
            print(f'Relay data inserted: {solar_to}, {house_from}, {power_solar}, {power_home}')

    def insert_anti_dust_data(self, operation):
        cursor = self._database.cursor()
        cursor.execute(f'INSERT INTO AntiDust (operation) VALUES ("{operation}")')
        self._database.commit()
        if cursor.rowcount > 0:
            print(f'AntiDust operation inserted')

    def insert_hvac_data(self, power, status, mode, target_temp):
        cursor = self._database.cursor()
        cursor.execute(f'INSERT INTO HVAC (power, status, mode, target_temp) VALUES ({power}, {status}, {mode}, {target_temp})')
        self._database.commit()
        if cursor.rowcount > 0:
            print(f'HVAC data inserted: {power}, {status}, {mode}, {target_temp}')

    # test method to get last num entities of each table
    def get_last_entries(self, table, num):
        cursor = self._database.cursor()
        cursor.execute(f'SELECT * FROM {table} ORDER BY timestamp DESC LIMIT {num}')
        return cursor.fetchall()
    
    def get_last_sensor_entries(self, sensor, num):
        cursor = self._database.cursor()
        cursor.execute(f'SELECT * FROM Sensors WHERE sensor = "{sensor}" ORDER BY timestamp DESC LIMIT {num}')
        return cursor.fetchall()

    def close(self):
        if self._database.is_connected():
            self._database.close()
            print("Database connection closed")
        else:
            print("Database connection was already closed")

# Create a shared instance of MySQLDB
HVAC_DB = MySQLDB()    


