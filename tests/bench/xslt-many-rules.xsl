<?xml version="1.0"?>
<!-- Many template rules (packet perf-15): one rule per element name of
     the XMark document, rules for attributes and text, applied to every
     node of the document. Each element writes the first letter of its
     name, each @id a "#", so the output shows the rule chosen for every
     element and attribute. -->
<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:template match="/">
    <r><xsl:apply-templates select="node()"/></r>
  </xsl:template>
  <xsl:template match="text">t<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="date">d<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="incategory">i<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="keyword">k<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="bold">b<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="emph">e<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="listitem">l<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="time">t<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="personref">p<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="increase">i<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="bidder">b<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="watch">w<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="name">n<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="description">d<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="quantity">q<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="interest">i<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="person">p<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="emailaddress">e<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="parlist">p<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="type">t<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="shipping">s<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="seller">s<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="payment">p<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="mailbox">m<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="location">l<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="itemref">i<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="item">i<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="happiness">h<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="author">a<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="annotation">a<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="to">t<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="mail">m<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="from">f<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="zipcode">z<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="street">s<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="country">c<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="city">c<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="address">a<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="profile">p<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="business">b<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="phone">p<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="homepage">h<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="watches">w<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="creditcard">c<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="start">s<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="open_auction">o<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="interval">i<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="initial">i<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="end">e<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="current">c<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="price">p<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="closed_auction">c<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="buyer">b<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="province">p<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="education">e<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="age">a<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="gender">g<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="reserve">r<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="privacy">p<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="category">c<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="edge">e<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="site">s<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="samerica">s<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="regions">r<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="people">p<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="open_auctions">o<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="namerica">n<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="europe">e<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="closed_auctions">c<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="catgraph">c<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="categories">c<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="australia">a<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="asia">a<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="africa">a<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="person[@id = 'person0']">P<xsl:apply-templates select="@* | node()"/></xsl:template>
  <xsl:template match="@id">#</xsl:template>
  <xsl:template match="@*"/>
  <xsl:template match="text()"/>
  <xsl:template match="*">?<xsl:apply-templates select="@* | node()"/></xsl:template>
</xsl:stylesheet>
