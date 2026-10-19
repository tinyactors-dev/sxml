<?xml version="1.0"?>
<!-- A key join in XSLT: for each person, the closed auctions they bought,
     through xsl:key and key(). -->
<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:key name="bought" match="closed_auction" use="buyer/@person"/>
  <xsl:template match="/">
    <bought>
      <xsl:for-each select="//person">
        <person id="{@id}" count="{count(key('bought', @id))}"/>
      </xsl:for-each>
    </bought>
  </xsl:template>
</xsl:stylesheet>
