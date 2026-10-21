<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:output method="html" indent="yes"/>
  <xsl:template match="/rfc">
    <html>
      <body>
        <h1>{front/title}</h1>
        <xsl:apply-templates select="middle/section"/>
      </body>
    </html>
  </xsl:template>
  <xsl:template match="section">
    <h2>{position()}. {name}</h2>
    <ul>
      <xsl:apply-templates select="t[@anchor]"/>
    </ul>
  </xsl:template>
  <xsl:template match="t">
    <li id="{@anchor}">{.}</li>
  </xsl:template>
</xsl:stylesheet>
