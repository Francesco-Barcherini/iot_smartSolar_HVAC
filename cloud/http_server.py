from cloud_app import flask_app
import config.app_config as conf

if __name__ == "__main__":
    print(f'Starting HTTP server on port {conf.HTTP_PORT}...')
    flask_app()
    print('HTTP server started')