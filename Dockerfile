FROM gcc:latest
COPY . /usr/src/app
WORKDIR /usr/src/app
RUN gcc -o app echo.c
CMD ["./app"]
