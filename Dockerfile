FROM gcc:latest

WORKDIR /app

COPY . .

RUN g++ -std=c++17 main.cpp -o nova_bank

CMD ["./nova_bank"]