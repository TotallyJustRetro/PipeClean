FROM node:22-alpine
ENV NODE_ENV=production
ENV PIPECLEAN_RELAY_BIND=0.0.0.0
ENV PIPECLEAN_RELAY_PORT=27800
WORKDIR /app
COPY server/relay.js ./relay.js
USER node
EXPOSE 27800
CMD ["node", "./relay.js"]
