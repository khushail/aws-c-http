#!/usr/bin/env bash
set -ex

# install njs
sudo apt install -y curl gnupg2 ca-certificates lsb-release ubuntu-keyring

curl https://nginx.org/keys/nginx_signing.key | gpg --dearmor \
    | sudo tee /usr/share/keyrings/nginx-archive-keyring.gpg >/dev/null
mkdir ~/.gnupg
gpg --dry-run --quiet --import --import-options import-show /usr/share/keyrings/nginx-archive-keyring.gpg
echo "deb [signed-by=/usr/share/keyrings/nginx-archive-keyring.gpg] \
http://nginx.org/packages/ubuntu `lsb_release -cs` nginx" \
    | sudo tee /etc/apt/sources.list.d/nginx.list

echo -e "Package: *\nPin: origin nginx.org\nPin: release o=nginx\nPin-Priority: 900\n" \
    | sudo tee /etc/apt/preferences.d/99nginx

sudo apt update
sudo apt install -y nginx nginx-module-njs


#Generate local ssl cert
openssl req -x509 -out localhost.crt -keyout localhost.key \
  -newkey rsa:2048 -nodes -sha256 \
  -subj '/CN=localhost' -extensions EXT -config <( \
   printf "[dn]\nCN=localhost\n[req]\ndistinguished_name = dn\n[EXT]\nsubjectAltName=DNS:localhost\nkeyUsage=digitalSignature\nextendedKeyUsage=serverAuth")

# copy config to etc/nginx/
sudo cp localhost.key localhost.crt /etc/nginx/

# copy the njs script
sudo cp -r ./tests/localhost/njs /etc/nginx/

# overwrite the njs configuration
sudo cp ./tests/localhost/nginx.conf /etc/nginx/

# test the config works
sudo nginx -t

# start the service
sudo service nginx start

# use curl to test if the server works
curl -v -k https://localhost:443/echo